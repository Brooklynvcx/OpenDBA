#ifndef __dba_hpp_included
#define __dba_hpp_included

#include <limits>
#include <thrust/sort.h>
#include <iostream>
#include <fstream>
#include <string>
#if defined(_WIN32)
	#include <Windows.h>
	extern "C"{
		#include "getopt.h"
	}
#else
	#include <unistd.h>
#endif

#include "exit_codes.hpp"
#include "gpu_utils.hpp"
#include "io_utils.hpp"
#include "cuda_utils.hpp"
#include "cpu_utils.hpp"
#include "dtw.hpp"
#include "clustering.cuh"
#include "limits.hpp" // for CUDA kernel compatible max()
#include "submodules/hclust-cpp/fastcluster.h"
#include "read_mode_codes.h"
#include "mem_export.h" // for in - memory model of dba result for return to programmatic callers to performDBA()

#define CLUSTER_ONLY 1
#define CONSENSUS_ONLY 2
#define CLUSTER_AND_CONSENSUS 3

using namespace cudahack; // for device-side numeric limits

template<typename T>
__host__ int* approximateMedoidIndices(T *gpu_sequences, size_t maxSeqLength, size_t num_sequences, size_t *sequence_lengths, char **sequence_names, int use_open_start, int use_open_end, char *output_prefix, double *cdist, int *memberships, cudaStream_t stream) {
	int deviceCount;
 	cudaGetDeviceCount(&deviceCount); CUERR("Getting GPU device count in medoid approximation method");

	unsigned int *maxThreads = getMaxThreadsPerDevice(deviceCount); // from cuda_utils.hpp
	// Pick the lowest common denominator 
       	dim3 threadblockDim(maxThreads[0], 1, 1);
	for(int i = 1; i < deviceCount; i++){
		if(maxThreads[i] < threadblockDim.x){
			threadblockDim.x = maxThreads[i];
		}
	}

	T **gpu_dtwPairwiseDistances = 0;
	cudaMallocHost(&gpu_dtwPairwiseDistances,sizeof(T *)*deviceCount);  CUERR("Allocating CPU memory for GPU DTW pairwise distances' pointers");

	size_t numPairwiseDistances = ARITH_SERIES_SUM(num_sequences-1); // arithmetic series of 1..(n-1)
	for(int i = 0; i < deviceCount; i++){
		cudaSetDevice(i);
		cudaMalloc(&gpu_dtwPairwiseDistances[i], sizeof(T)*numPairwiseDistances); CUERR("Allocating GPU memory for DTW pairwise distances");
	}
	T *cpu_dtwPairwiseDistances = 0;
	cudaMallocHost(&cpu_dtwPairwiseDistances, sizeof(T)*numPairwiseDistances); CUERR("Allocating page locked CPU memory for DTW pairwise distances");

	int priority_high, priority_low, descendingPriority;
	cudaDeviceGetStreamPriorityRange(&priority_low, &priority_high);
	descendingPriority = priority_high;
	// To save on space while still calculating all possible DTW paths, we process all DTWs for one sequence at the same time.
        // So allocate space for the dtwCost to get to each point on the border between grid vertical swaths of the total cost matrix.
	int dotsPrinted = 0;
	for(size_t seq_index = 0; seq_index < num_sequences-1; seq_index+=deviceCount){
		// An issue can pop up with extremely long sequences that we are typically launching a kernel every 256 or 1024 sequence elements, and an async copy.
		// So, a stream can get 900+ kernel launches queued up in it once it becomes 450K elements long. The kernel launch queue for a stream is 
		// not specifically defined, but with near 1000 launches queued up, another kernel launch will sit synchronously and wait for something to come off the queue.
		// TODO: To avoid this bottleneck, we should distribute the launches into many queues. Each kernel launched can only run on one streaming multi-processor
		// at a time and a V100 card for example has 80 of these. If we provide many streams and launch into all of them we utilize the GPU cards more effectively
		// at the cost of only 2*Y where Y is the length of the vertical sequence in the DTW cost matrix.  We can do this because we don't store the full cost matrix, 
		// only the leading edge between kernel calls calculating a 256 or 1024 column wide swath of it.
		//
		// The most effective throughput technique is a breadth first distrbution of the sequence pair comparisons across available devices.
		// Then you can start using multiple streams per device. 
		size_t dtwCostSoFarSize[deviceCount];
		T *dtwCostSoFar[deviceCount];
		T *newDtwCostSoFar[deviceCount];
		cudaStream_t seq_stream[deviceCount]; 
		for(int currDevice = 0; currDevice < deviceCount && seq_index + currDevice < num_sequences-1; currDevice++){
			cudaSetDevice(currDevice);
			size_t current_seq_length = sequence_lengths[seq_index+currDevice];
			// We are allocating each time rather than just once at the start because if the sequences have a large
                	// range of lengths and we sort them from shortest to longest we will be allocating the minimum amount of
			// memory necessary.
        		dim3 gridDim((num_sequences-seq_index-currDevice-1), 1, 1);
			dtwCostSoFarSize[currDevice] = sizeof(T)*current_seq_length*gridDim.x;
			size_t freeGPUMem;
			size_t totalGPUMem;
			cudaMemGetInfo(&freeGPUMem, &totalGPUMem);	
			if(freeGPUMem < dtwCostSoFarSize[currDevice]){
				//std::this_thread::sleep_for(std::chrono::seconds(1));
				#ifdef _WIN32
					Sleep(1000);
				#else
					usleep(1000000);
				#endif
				// This is a super tight loop for the all-vs-all, not willing to move to managed memory just now.
				std::cerr << "Note: Insufficient free GPU memory (" << freeGPUMem << " bytes of total " << totalGPUMem << 
					     ") on device " << currDevice << 
					     " for initial medoid calculation (need " << dtwCostSoFarSize[currDevice] << "), calculation speed may suffer." << std::endl;
			}
			cudaMallocManaged(&dtwCostSoFar[currDevice], dtwCostSoFarSize[currDevice]);  CUERR("Allocating managed memory for DTW pairwise distance intermediate values");
			cudaMallocManaged(&newDtwCostSoFar[currDevice], dtwCostSoFarSize[currDevice]); CUERR("Allocating managed memory for new DTW pairwise distance intermediate values");

			// Make calls to DTWDistance serial within each seq, but allow multiple seqs on the GPU at once.
			cudaStreamCreateWithPriority(&seq_stream[currDevice], cudaStreamNonBlocking, descendingPriority);
			if(descendingPriority < priority_low){
				descendingPriority++;
			}
		} // end for setup of each device

		// TODO: A seemingly unique approach to speeding up the cumulative cost calculation of very large DTW matrix traversal is to calculate the leading swath column (dtwCostSoFar)
		// from *both ends* of the matrix in parallel, then if we carefully picked column n/2 as the dtwCostSoFar going forward and column n/2+1 as the dtwCostSoFar going 
		// backward (given n columns in the DTW matrix), we can run a special single column path choice that calculates the minimal total cost "meeting in the middle". This is 
		// somewhat akin to how lightning actually can connect the least resistent "stepped leader" bolt emanating ground-to-sky with the return bolt coming
		// downward sky-to-ground. Hence I'm calling this technique "Lightning DTW".
		// e.g. Imagine a DTW matrix with 9 rows and 2200 columns. The last forward dtwCoSoFar column is 1100, the last backward dtwCostSoFar column is 1101.
		// Calculating the total minimal traversal cost calculation might look as follows;
		//
		// fwd_column_1100_cost  rev_column_1101_cost  DTW_steps_column_1100_to_110_through_this_row  total_cost_for_traversal_through_this_row_at_column_1101
		// row9             100  110                   DIAG                                           210    
		// row8             100  110                   DIAG                                           163
		// row7              53  110                   DIAG                                           138
		// row6              28  115                   RIGHT                                          143
		// row5              46  120                   DIAG                                           150
		// row4              30  105                   RIGHT                                          135
		// row3              75  107                   RIGHT                                          182
		// row2              93  99                    RIGHT                                          192
		// row1             100  110                   RIGHT                                          210
		//                            min(total_cost_for_traversal_through_this_row_at_column_1101) = 135
		//
		// The moves are necessarily diagonal or right because up moves on column 1100 (or 'down' on 1101) are already baked into the cumulative costs. Note that for row 9, all things be equal we pick 
		// diagonal moves over right moves, though the "choice" is immaterial in simple total cost calculation.
		
		for(size_t offset_within_seq = 0; offset_within_seq < maxSeqLength; offset_within_seq += threadblockDim.x){
			for(int currDevice = 0; currDevice < deviceCount && seq_index + currDevice < num_sequences-1; currDevice++){
				cudaSetDevice(currDevice);
        			dim3 gridDim((num_sequences-seq_index-currDevice-1), 1, 1);
				// We have a circular buffer in shared memory of three diagonals for minimal proper DTW calculation, and an array for an inline findMin()
        			int shared_memory_required = threadblockDim.x*3*sizeof(T);
				// Null unsigned char pointer arg below means we aren't storing the path for each alignment right now.
				// And (T *) 0, (size_t) 0, (T *) 0, (size_t) 0, means that the sequences to be compared will be defined by seq_index (1st, Y axis seq) and the block x index (2nd, X axis seq)
				DTWDistance<<<gridDim,threadblockDim,shared_memory_required,seq_stream[currDevice]>>>((T *) 0, (size_t) 0, (T *) 0, (size_t) 0, seq_index+currDevice, offset_within_seq, gpu_sequences, maxSeqLength,
										num_sequences, sequence_lengths, dtwCostSoFar[currDevice], newDtwCostSoFar[currDevice], 
										(unsigned char *) 0, (size_t) 0, gpu_dtwPairwiseDistances[currDevice], 
										use_open_start, use_open_end); CUERR("DTW vertical swath calculation with cost storage");
				cudaMemcpyAsync(dtwCostSoFar[currDevice], newDtwCostSoFar[currDevice], dtwCostSoFarSize[currDevice], cudaMemcpyDeviceToDevice, seq_stream[currDevice]); CUERR("Copying DTW pairwise distance intermediate values");
				if(offset_within_seq+threadblockDim.x >= maxSeqLength){
					dotsPrinted = updatePercentageComplete(seq_index+currDevice, num_sequences-1, dotsPrinted);
				}
			}
		} 
		// Will cause memory to be freed in callback after seq DTW completion, so the sleep_for() polling above can 
		// eventually release to launch more kernels as free memory increases (if it's not already limited by the kernel grid block queue).
		for(int currDevice = 0; currDevice < deviceCount && seq_index + currDevice < num_sequences-1; currDevice++){
			addStreamCleanupCallback(dtwCostSoFar[currDevice], newDtwCostSoFar[currDevice], 0, seq_stream[currDevice]);
		}
	}
	std::cerr << std::endl;
        cudaFreeHost(maxThreads); CUERR("Freeing CPU memory for device thread properties");
	// TODO: use a fancy cleanup thread barrier here so that multiple DBAs could be running on the same device and not interfere with each other at this step.
	for(int i = 0; i < deviceCount; i++){
                cudaSetDevice(i);
		cudaDeviceSynchronize(); CUERR("Synchronizing CUDA device after all DTW calculations");
	}

	T *dtwSoS;
	// Technically dtsSoS does not need to be page locked as it doesn't get copied to the GPU, but we're futureproofing it and it's going 
	// to be in an existing page most likely anyway, given all the cudaMallocHost() calls before this.
	cudaMallocHost(&dtwSoS, sizeof(T)*num_sequences); CUERR("Allocating CPU memory for DTW pairwise distance sums of squares");
	std::memset(dtwSoS, 0, sizeof(T)*num_sequences);
        // Reassemble the whole pair matrix (upper right only) from the rows that each device processed.
	for(int i = 0; i < deviceCount; i++){
		cudaSetDevice(i);
		for(int j = i; j < num_sequences-1; j+= deviceCount){
			size_t offset = PAIRWISE_DIST_ROW(j, num_sequences);
			cudaMemcpy(cpu_dtwPairwiseDistances + offset, 
                                   gpu_dtwPairwiseDistances[i] + offset, 
				   sizeof(T)*(num_sequences-j-1), cudaMemcpyDeviceToHost); CUERR("Copying DTW pairwise distances to CPU");
		}
	}

	size_t index_offset = 0;
	T max_distance = (T) 0;
	std::ofstream mats((std::string(output_prefix)+std::string(".pair_dists.txt")).c_str());
	for(size_t seq_index = 0; seq_index < num_sequences-1; seq_index++){
		mats << sequence_names[seq_index];
		for(size_t pad = 0; pad < seq_index; ++pad){
			mats << "\t";
		}
		mats << "\t0"; //self-distance
		for(size_t paired_seq_index = seq_index + 1; paired_seq_index < num_sequences; ++paired_seq_index){
			T dtwPairwiseDistanceSquared = cpu_dtwPairwiseDistances[index_offset+paired_seq_index-seq_index-1];
			if(max_distance < dtwPairwiseDistanceSquared){
				max_distance = dtwPairwiseDistanceSquared;
			}	
			mats << "\t" << dtwPairwiseDistanceSquared;
			dtwPairwiseDistanceSquared *= dtwPairwiseDistanceSquared;
			dtwSoS[seq_index] += dtwPairwiseDistanceSquared;
			dtwSoS[paired_seq_index] += dtwPairwiseDistanceSquared;
		}
		index_offset += num_sequences - seq_index - 1;
		mats << std::endl;
	}
	
	// If sequences are the same then max_distance would be 0. We set it to 1 because any number divided by 1 will still be itself. Saves us from dividing by 0 later.
	if(max_distance == 0) max_distance = 1;
	// Last line is pro forma as all pair distances have already been printed
	mats << sequence_names[num_sequences-1];
	for(size_t pad = 0; pad < num_sequences; ++pad){
                mats << "\t";
        }
	mats << "0" << std::endl;

	// Don't allocate to the heap, this number can get big, and not enough heap space, and cause a seg fault when accessed
	double *cpu_double_dtwPairwiseDistances = 0;
	cpu_double_dtwPairwiseDistances = (double *) calloc(ARITH_SERIES_SUM(num_sequences-1), sizeof(double));
	if(!cpu_double_dtwPairwiseDistances){ // should only really happen if allocating > 2^32 on a 32 but system
		std::cerr << "Cannot allocate pairwise distance matrix for medoid clustering" << std::endl;
		exit(CANNOT_ALLOCATE_PAIRWISE_DIST_ARRAY);
	}
	for(int i = 0; i < ARITH_SERIES_SUM(num_sequences-1); i++){
		cpu_double_dtwPairwiseDistances[i] = ((double) cpu_dtwPairwiseDistances[i])/((double) max_distance); // move into [0,1] range
	}

	// A dataset may contain logical subdivisions of sequences (e.g. classic UCR time series "gun vs. no-gun", or different 
	// transcripts in Oxford Nanopore Technologies direct RNA data), in which case it can be useful
	// to generate average sequences for each of the subdivisions rather than merging their unique characteristics.
	int* merge = new int[2*(num_sequences-1)];
	double* height = new double[num_sequences-1];
	hclust_fast(num_sequences, cpu_double_dtwPairwiseDistances, HCLUST_METHOD_COMPLETE, merge, height);
	free(cpu_double_dtwPairwiseDistances);

	// Three possible strategies for clustering
	if(*cdist > 1){ // assume you want to do k-means clustering
		int new_k = *cdist;
		if(new_k > num_sequences){
			// Everything is in its own cluster
			new_k = num_sequences;
		}
		std::cerr << std::endl << "Using K-means clustering (excluding singletons)" << std::endl;
		// Exclude any singletons as being considered "clusters"
		int num_multimember_clusters;
		do{
			cutree_k(num_sequences, merge, new_k, memberships);
			int* num_members_per_cluster = new int[new_k](); // zero-initialized
			for(int i = 0; i < num_sequences; i++){
				num_members_per_cluster[memberships[i]]++;
			}
			num_multimember_clusters = 0;
			for(int i = 0; i < new_k; i++){
				if(num_members_per_cluster[i] > 1){
					num_multimember_clusters++;
				}
                        }
			//std::cerr << "Found " << num_multimember_clusters << " multicluster members with K set to " << new_k << std::endl;
			delete[] num_members_per_cluster; // overkill maybe?
			new_k += ((int) *cdist) - num_multimember_clusters; // adjust K to compensate for singletons eating up real cluster space
		} while(num_multimember_clusters < ((int) *cdist) && new_k < num_sequences);
		std::cerr << "Final K to compensate for singletons: " << new_k << std::endl;
		
	}
        else if(*cdist == 1){
		// Special case for 1, always everything in one cluster. Avoids cutree_cdist split of two-leaf-only dendrograms
		// and other simple topologies with branch length 1.
		for(int i = 0; i < num_sequences; i++){
			memberships[i] = 0;
		}
	}
	else if(*cdist >= 0){
		// Stop clustering at step with cluster distance >= cdist
		std::cerr << std::endl << "Using dendrogram fixed height clustering cutoff" << std::endl;
		cutree_cdist(num_sequences, merge, height, *cdist, memberships);
	}
	else{ 	/* TODO
		// Negative number means we want to use permutation statistics supported cluster building
		float cluster_p_value = 0.05;
		merge_clusters(gpu_sequences, sequence_lengths, num_sequences, cpu_dtwPairwiseDistances, merge, cluster_p_value, 
			       memberships, use_open_start, use_open_end, stream); 
		*/
	}
	delete[] merge;
	delete[] height;

	int num_clusters = 1;
	for(int i = 0; i < num_sequences; i++){
		if(memberships[i] >= num_clusters){
			num_clusters = memberships[i]+1;
		}
	}
	std::cerr << "There are " << num_clusters << " clusters" << std::endl;
	int *medoidIndices = new int[num_clusters];

	T *clusterDtwSoS = num_clusters == 1 ? dtwSoS : new T[num_sequences](); // will use some portion of this max for each cluster
	for(int currCluster = 0; currCluster < num_clusters; currCluster++){
		std::cerr << "Processing cluster " << currCluster;
		int num_cluster_members = 0;
		for(size_t i = 0; i < num_sequences; ++i){
			if(memberships[i] == currCluster){
				num_cluster_members++;
			}
		}
		int *clusterIndices = new int[num_cluster_members];
		std::cerr << " membership=" << num_cluster_members << ", ";
		int cluster_cursor = 0;
		for(size_t i = 0; i < num_sequences; ++i){
			if(memberships[i] == currCluster){
				clusterIndices[cluster_cursor++] = i;
			}
		}
		if(num_clusters > 1){
			for(size_t i = 0; i < num_cluster_members - 1; ++i){
				// Where in the upper right matrix we are i.e. the whole matrix minus what down and to the right of this row's start
				int index_offset = PAIRWISE_DIST_ROW(i, num_sequences); 
				for(size_t j = i + 1; j < num_cluster_members; ++j){
					T paired_distance = cpu_dtwPairwiseDistances[index_offset+j-i-1];
					clusterDtwSoS[i] += paired_distance*paired_distance;
					clusterDtwSoS[j] += paired_distance*paired_distance;
				}
			}
		}
		int medoidIndex = -1;
		// Pick the smallest squared distance across all the sequences in this cluster.
		if(num_cluster_members > 2){
			T lowestSoS = std::numeric_limits<T>::max();
			for(size_t i = 0; i < num_cluster_members; ++i){
				if (clusterDtwSoS[i] < lowestSoS) {
					medoidIndex = clusterIndices[i];
					lowestSoS = clusterDtwSoS[i];
				}
			}
		} 
		else if(num_cluster_members == 2){
			// Pick the longest sequence that contributed to the cumulative distance if we only have 2 sequences
			medoidIndex = sequence_lengths[clusterIndices[0]] > sequence_lengths[clusterIndices[1]] ? clusterIndices[0] : clusterIndices[1];
		}
		else{	// Single member cluster
			medoidIndex = clusterIndices[0];
		}
		// Sanity check
		if(medoidIndex == -1){
			std::cerr << "Logic error in medoid finding routine, please e-mail the developer (gordonp@ucalgary.ca)." << std::endl;
			exit(MEDOID_FINDING_ERROR);
		}
		medoidIndices[currCluster] = medoidIndex;
		std::cerr << "medoid is " << medoidIndex << std::endl;
		delete[] clusterIndices;
	}
	if(num_clusters != 1){
		delete[] clusterDtwSoS;
	}
	cudaFreeHost(dtwSoS); CUERR("Freeing CPU memory for DTW pairwise distance sum of squares");
	cudaFreeHost(cpu_dtwPairwiseDistances); CUERR("Freeing page locked CPU memory for DTW pairwise distances");
	for(int i = 0; i < deviceCount; i++){
		cudaSetDevice(i); // not sure this is necessary?
		cudaFree(gpu_dtwPairwiseDistances[i]); CUERR("Freeing GPU memory for DTW pairwise distances");
	}
	cudaFreeHost(gpu_dtwPairwiseDistances); CUERR("Freeing CPU memory for GPU DTW pairwise distances' pointers");
	mats.close();
	//std::cerr << "Returning medoid indices" << std::endl;
	return medoidIndices;
}

/**
 * Employ the backtracing algorithm through the path matrix to find the optimal DTW path for 
 * sequence (indexed by i) vs. centroid (indexed by j), accumulating the sequence value at each centroid element 
 * for eventual averaging on the host side once all sequences have been through this same process on the GPU
 * (there is no point in doing the whole thing host side since copying all the path matrices back to the CPU would be slower, occupy more CPU memory,
 * and this kernel execution can be interleaved with memory waiting DTW calculation kernels to reduce turnaround time).
 */
template<typename T>
__global__
void updateCentroid(T *seq, T *centroidElementSums, unsigned int *nElementsForMean, unsigned char *pathMatrix, size_t pathColumns, size_t pathRows, size_t pathMemPitch, int flip_seq_order, int column_offset = 0, int *stripe_rows = 0){
	// Backtrack from the end of both sequences to the start to get the optimal path.
	int j = pathColumns - 1;
	int i = pathRows - 1; 
	if(flip_seq_order){
		size_t tmp = i;
		i = j;
		j = tmp;
	}
	// This is used in stripe backtracing mode, to change from global seq/centroid coords to local stripe coords used in the stripe being processed by this call.
	//j -= column_offset; 
	// If we set stripe mode, the height of the effective matrix is taken from GPU memory from last call to this function.
	if(stripe_rows){
		i = ((int) *stripe_rows) - 1;
	}

	unsigned char move = pathMatrix[pitchedCoord(j,i,pathMemPitch)];
	while (j >= 0 && move != NIL && move != NIL_OPEN_RIGHT) {
		// Don't count open end moves as contributing to the consensus.
		if(move != OPEN_RIGHT){ 
			// flip_seq_order indicates that the consensus is on the Y axis for this path matrix rather than the X axis.
			atomicAdd(&centroidElementSums[flip_seq_order ? i : j+column_offset], seq[flip_seq_order ? j+column_offset : i]);
			atomicInc(&nElementsForMean[flip_seq_order ? i : j+column_offset], numeric_limits<unsigned int>::max());
		}
		// moveI and moveJ are defined device-side in dtw.hpp
		i += (size_t) moveI[move];
		j += (size_t) moveJ[move];
		move = pathMatrix[pitchedCoord(j,i,pathMemPitch)];
	}
	// If the path matrix & moveI & moveJ are sane, we will necessarily be at i == 0, j == 0 when the backtracking finishes.
	if(column_offset == 0){
	        if(j != 0 || j != 0){
			// Executing a PTX assembly language trap is the closest we get to throwing an exception in a CUDA kernel. 
			// The next call to CUERR() will report "an illegal instruction was encountered".
			asm("trap;"); 
	        }
		if(move != NIL_OPEN_RIGHT) {
			atomicAdd_system(&centroidElementSums[0], seq[0]);
			atomicInc_system(&nElementsForMean[0], numeric_limits<unsigned int>::max());
		}
	}
	else if(j != -1 || i < 0){ // if in stripe mode we should have traversed past the left edge, but not past the bottom
		asm("trap;");
	}
	if(stripe_rows){ // update the stripe height for the net call to be only as high as where the backtrace got us
		*stripe_rows = i + 1;
	}
}

/**
 * Returns the delta (max movement of a single point in the centroid) after update.
 *
 * @param C a gpu-side centroid sequence array
 *
 * @param updatedMean a cpu-side location for the result of the DBAUpdate to the centroid sequence
 */
template<typename T>
__host__ double 
DBAUpdate(T *C, size_t centerLength, T **sequences, char **sequence_names, size_t num_sequences, size_t *sequence_lengths, int use_open_start, int use_open_end, T *updatedMean, std::string output_prefix, cudaStream_t stream) {

	T *gpu_centroidAlignmentSums;
	// cudaSetDevice(#); not strictly necessary here since all the consensus variables are managed memory, which in the unified memory model are accessible across all devices
	// we do require compute capability 6.0+ so that atomicAdd "system" flavor works across devices
	cudaMallocManaged(&gpu_centroidAlignmentSums, sizeof(T)*centerLength); CUERR("Allocating GPU memory for barycenter update sequence element sums");
	cudaMemset(gpu_centroidAlignmentSums, 0, sizeof(T)*centerLength); CUERR("Initialzing GPU memory for barycenter update sequence element sums to zero");
	
	T *cpu_centroid;
        cudaMallocHost(&cpu_centroid, sizeof(T)*centerLength); CUERR("Allocating CPU memory for incoming centroid");
	cudaMemcpy(cpu_centroid, C, sizeof(T)*centerLength, cudaMemcpyDeviceToHost); CUERR("Copying incoming GPU centroid to CPU");

	int deviceCount;
        cudaGetDeviceCount(&deviceCount); CUERR("Getting GPU device count in DBA update function");
#if DEBUG == 1
	// Device parallelism is not compatible with debug printing of intermediate path cost matrix columns
	deviceCount = 1;
#endif
	// TODO: parallelize within devices
        unsigned int *maxThreads = getMaxThreadsPerDevice(deviceCount);
	// For testing purposes, see if 1024 is faster than maxThreads
        for(int i = 0; i < deviceCount; i++){
                maxThreads[i] = 1024;
        }

	unsigned int *nElementsForMean, *cpu_nElementsForMean; // Using unsigned int rather than size_t so we can use CUDA atomic operations on their GPU counterparts.
	cudaMallocManaged(&nElementsForMean, sizeof(unsigned int)*centerLength); CUERR("Allocating GPU memory for barycenter update sequence pileup");
	cudaMemset(nElementsForMean, 0, sizeof(unsigned int)*centerLength); CUERR("Initialzing GPU memory for barycenter update sequence pileup to zero");
	cudaMallocHost(&cpu_nElementsForMean, sizeof(unsigned int)*centerLength); CUERR("Allocating CPU memory for barycenter sequence pileup");

        int priority_high, priority_low, descendingPriority;
        cudaDeviceGetStreamPriorityRange(&priority_low, &priority_high);
        descendingPriority = priority_high;

        // Allocate space for the dtwCost to get to each point on the border between grid vertical swaths of the total cost matrix against the consensus C.
	// Generate the path matrix though for each sequence relative to the centroid, and update the centroid means accordingly.

	int dotsPrinted = 0;
       	size_t current_seq_length[deviceCount];
	int flip_seq_order[deviceCount]; // boolean
        cudaStream_t seq_stream[deviceCount];
	T **dtwCostSoFar = new T * [deviceCount](); // parentheses zero-initializes
        T **newDtwCostSoFar = new T * [deviceCount]();
	int *gpu_backtrace_rows[deviceCount] = {}; // for consensus update: backtracking indicator of first (vertical) seq in the DTW cost matrix for use with stripe mode
       	size_t pathPitch[deviceCount];
       	unsigned char *pathMatrix[deviceCount] = {};
	bool usingStripePath[deviceCount];
	int cpu_backtrace_rows[deviceCount] = {}; // for printing DTW path: backtracking indicator of first (vertical) seq in the DTW cost matrix for use with stripe mode
	std::ofstream **cpu_backtrace_outputstream = new std::ofstream *[deviceCount]; // for printing DTW path: defined outside print method so that we can print in multiple parts during stripe mode
	unsigned char **cpu_stepMatrix = new unsigned char *[deviceCount]; // for client side copy of DTW path matrix that we're going to print
	for(size_t seq_index = 0; seq_index < num_sequences; seq_index++){
                int currDevice = seq_index%deviceCount;
                cudaSetDevice(currDevice);
                dim3 threadblockDim(maxThreads[currDevice], 1, 1);
                current_seq_length[currDevice] = sequence_lengths[seq_index];

                // We are allocating each time rather than just once at the start because if the sequences have a large
                // range of lengths and we sort them from shortest to longest we will be allocating the minimum amount of
                // memory necessary.
                size_t pathMatrixSize = sizeof(unsigned char)*current_seq_length[currDevice]*centerLength;
                size_t freeGPUMem;
                size_t totalGPUMem;
                size_t dtwCostSoFarSize = sizeof(T)*current_seq_length[currDevice];
		flip_seq_order[currDevice] = 0;
		if(use_open_end && centerLength < current_seq_length[currDevice]){
			flip_seq_order[currDevice] = 1;
			dtwCostSoFarSize = sizeof(T)*centerLength;
		}
                // Make calls to DTWDistance serial within each seq, but allow multiple seqs on the GPU at once.
                cudaStreamCreateWithPriority(&seq_stream[currDevice], cudaStreamNonBlocking, descendingPriority); CUERR("Creating prioritized CUDA stream");
                if(descendingPriority < priority_low){
                        descendingPriority++;
                }

		std::string path_filename = output_prefix+std::string(".path")+std::to_string(seq_index)+".txt";
		cpu_backtrace_outputstream[currDevice] = new std::ofstream(path_filename);
                if(! (*(cpu_backtrace_outputstream[currDevice])).is_open()){
                        std::cerr << "Cannot write to " << path_filename << std::endl;
                        return CANNOT_WRITE_DTW_PATH_MATRIX;
                }
	
		// If there is insufficient GPU memory is available for the path matrix, switch to an alternative 'stripe' mode where instead of 
		// storing all the path choices made, we don't store any during the forward pass through the cost calculations,
		// but we do store the leading column X (typically 1/1024th of the full matrix if threadblocks are 1024 wide) of all swaths calculated (kernel calls made) 
		// so we can back track the relevant parts of the path matrix by recalculating each swath up to the existing backtrack Y location. 
		// This will cost on average 1.5x the total normal calculations, but occupy 1/256th (depending on datatype used, 
		// e.g. 1/1024 x 4 bytes per float vs 1 byte per path element). This allows a 1M x 1M full (unbanded) DTW path calculation in ~4GB of GPU RAM
		// rather than an impractical 1TB. This is much more efficient than using classic DTW full path matrix and managed memory where the intensive reads and writes across
		// the CPU bus will slow us down considerably more than the 1.5x GPU-only compute cost.
		usingStripePath[currDevice] = false;
                cudaMemGetInfo(&freeGPUMem, &totalGPUMem);
                if(freeGPUMem < dtwCostSoFarSize+pathMatrixSize*1.05){ // assume pitching could add up to 5%
			pathMatrixSize = 0;
			usingStripePath[currDevice] = true;
			// Set up the stripe vertical index once if we're in that mode
			if(gpu_backtrace_rows[currDevice] == 0){
				cudaMallocManaged(&gpu_backtrace_rows[currDevice], sizeof(int));  CUERR("Allocating a single int for striped GPU backtrace vertical index");
			}
			// We take up a lot more cost matrix space (X*Y/1024*4 for float) than normal mode (2*Y*4), but still less overall as we no longer allocate path matrix of (X*Y)
			if(flip_seq_order[currDevice]){
				int l = (int) centerLength;
				cudaMemcpy(gpu_backtrace_rows[currDevice], &l, sizeof(int), cudaMemcpyHostToDevice);  
				CUERR("Running transfer of a flipped vertical index (a size_t) to the GPU in stripe mode");
				dtwCostSoFarSize = sizeof(T)*centerLength*(std::ceil(((float)current_seq_length[currDevice])/threadblockDim.x));
			}
			else{
				int l = (int) current_seq_length[currDevice];
				cudaMemcpy(gpu_backtrace_rows[currDevice], &l, sizeof(int), cudaMemcpyHostToDevice);  
				CUERR("Running transfer of vertical index (a size_t) to the GPU in stripe mode");
				dtwCostSoFarSize = sizeof(T)*current_seq_length[currDevice]*(std::ceil(((float)centerLength)/threadblockDim.x)); // because the last stripe will report out the full cost at the rightmost column of the full matrix (though only top-row value is correct since UP is not enforced)
			}
			// Height of the cost matrix. Default is centroid on the X axis, flip (done for computational efficiency of open_right move 
			// on longer seq or the pair) is centroid on Y axis.
			cpu_backtrace_rows[currDevice] = flip_seq_order[currDevice] ? centerLength : current_seq_length[currDevice];
                }

		dotsPrinted = updatePercentageComplete(seq_index+1, num_sequences, dotsPrinted);

		if(usingStripePath[currDevice]){
			// In the case of a truly massive path matrix or a tiny GPU memory pool, fall back gracefully to using the stripe mode with managed memory
			// where bits will be loaded in and out of page locked CPU RAM to the GPU (at some cost to performance).
			cudaMallocManaged(&dtwCostSoFar[currDevice], dtwCostSoFarSize);  CUERR("Allocating managed memory for DTW pairwise distance striped intermediate values in DBA update");
			// TODO: for now, we have only one process per device so not necessary, 
			// but in future if multithreading per device use cudaStreamAttachMemAsync() to reduce memory access barriers.
			pathMatrix[currDevice] = 0; // this will get populated later as a small matrix stripe for recalc and backtracking, after all the cost DTW calculations for this seq are done
		}
		else{ // "Normal" full path matrix calculation
                	cudaMalloc(&dtwCostSoFar[currDevice], dtwCostSoFarSize);  CUERR("Allocating GPU memory for DTW pairwise distance intermediate values in DBA update");
                	cudaMalloc(&newDtwCostSoFar[currDevice], dtwCostSoFarSize);  CUERR("Allocating GPU memory for new DTW pairwise distance intermediate values in DBA update");
			// Under the assumption that long sequences have the same or more information than the centroid, flip the DTW comparison so the centroid has an open end.
			// Otherwise you're cramming extra sequence data into the wrong spot and the DTW will give up and choose an all-up then all-open right path instead of a diagonal,
			// which messes with the consensus building.
			// Column major allocation x-axis is 2nd seq
			// NB: skipping this potentially large memory allocation step if we're using striped mode
        		if(flip_seq_order[currDevice]){
				cudaMallocPitch(&pathMatrix[currDevice], &pathPitch[currDevice], current_seq_length[currDevice], centerLength); CUERR("Allocating pitched GPU memory for centroid:sequence path matrix");
			}
			else{
				cudaMallocPitch(&pathMatrix[currDevice], &pathPitch[currDevice], centerLength, current_seq_length[currDevice]); CUERR("Allocating pitched GPU memory for sequence:centroid path matrix");
			}
		}

		int dtw_x_limit = flip_seq_order[currDevice] ? current_seq_length[currDevice] : centerLength;
#if DEBUG == 1
		int dtw_y_limit = flip_seq_order[currDevice] ? centerLength : current_seq_length[currDevice];
		std::string cost_filename = std::string("costmatrix")+"."+std::to_string(seq_index);
		std::ofstream cost(cost_filename);
		if(!cost.is_open()){
			std::cerr << "Cannot write to " << cost_filename << std::endl;
			return CANNOT_WRITE_DTW_PATH_MATRIX;
		}	
#endif
		size_t PARAM_NOT_USED = 0;
                // We have a circular buffer in shared memory of three diagonals for minimal proper DTW calculation using White-Neely step pattern.
                int shared_memory_required = threadblockDim.x*3*sizeof(T);
                for(size_t offset_within_seq = 0; offset_within_seq < dtw_x_limit; offset_within_seq += threadblockDim.x){
			T *existingCosts = dtwCostSoFar[currDevice];
			T *newCosts = newDtwCostSoFar[currDevice];
			if(usingStripePath[currDevice]){ // In striped mode we store the result of every swath computed, so we move further into a larger cost buffer rather than recycling a smaller one.
				newCosts = dtwCostSoFar[currDevice] + offset_within_seq/threadblockDim.x*(flip_seq_order[currDevice] ? centerLength : current_seq_length[currDevice]);
				// In striped mode the existing costs will point to unused nonsense for the first (leftmost) swath of the cost matrix.
				existingCosts = newCosts - (flip_seq_order[currDevice] ? centerLength : current_seq_length[currDevice]);
			}
			// 0 arg here means that we are not storing the pairwise distance (total cost) between the sequences back out to global memory.
                        if(flip_seq_order[currDevice]){
				// Specify both the first and second sequences explicitly (seq_index will actually be ignored)
				DTWDistance<<<1,threadblockDim,shared_memory_required,seq_stream[currDevice]>>>(C, centerLength, sequences[seq_index], current_seq_length[currDevice], 
						PARAM_NOT_USED, offset_within_seq, (T *)PARAM_NOT_USED, PARAM_NOT_USED, num_sequences, (size_t *)PARAM_NOT_USED, 
						existingCosts, newCosts, 
						pathMatrix[currDevice], pathPitch[currDevice], (T *) PARAM_NOT_USED, use_open_start, use_open_end); CUERR("Flipped consensus DTW vertical swath calculation with path storage");
				if(!usingStripePath[currDevice]){ // recycling cost buffers in full path matrix mode
					cudaMemcpyAsync(existingCosts, newCosts, dtwCostSoFarSize, cudaMemcpyDeviceToDevice, seq_stream[currDevice]); CUERR("Copying DTW pairwise distance intermediate values with flipped sequence order");
				}
			}
			else{
				// Specify both the first and second sequences explicitly (seq_index will actually be ignored)
				DTWDistance<<<1,threadblockDim,shared_memory_required,seq_stream[currDevice]>>>(sequences[seq_index], current_seq_length[currDevice], C, centerLength, 
						PARAM_NOT_USED, offset_within_seq, (T *)PARAM_NOT_USED, PARAM_NOT_USED, num_sequences, (size_t *) PARAM_NOT_USED, 
						existingCosts, newCosts, 
						pathMatrix[currDevice], pathPitch[currDevice], (T *) PARAM_NOT_USED, use_open_start, use_open_end); CUERR("Sequence DTW vertical swath calculation with path storage");
				if(!usingStripePath[currDevice]){ // recycling cost buffers in full path matrix mode
					cudaMemcpyAsync(existingCosts, newCosts, dtwCostSoFarSize, cudaMemcpyDeviceToDevice, seq_stream[currDevice]); CUERR("Copying DTW pairwise distance intermediate values without flipped sequence order");
				}
			}
#if DEBUG == 1
			T *hostCosts = newCosts;
			if(!usingStripePath[currDevice]){ // need to grab from the device memory, i.e. the pointer isn't managed
				cudaMallocHost(&hostCosts, dtwCostSoFarSize); CUERR("Allocating host memory for  debug print statements of sequence-centroid DTW cost matrix");
				cudaMemcpyAsync(hostCosts, newCosts, dtwCostSoFarSize, cudaMemcpyDeviceToHost, seq_stream[currDevice]); CUERR("Copying DTW pairwise distance intermediate values from device to host debug printing");
			}
			cudaStreamSynchronize(seq_stream[currDevice]);  CUERR("Synchronizing prioritized CUDA stream mid-path for debug output");
			for(int i = 0; i < dtw_y_limit; i++){
				cost << hostCosts[i] << ", ";
			}
			cost << std::endl;
#endif
                }
#if DEBUG == 1
		cost.close();
#endif
		if(!usingStripePath[currDevice]){
			updateCentroid<<<1,1,0,seq_stream[currDevice]>>>(sequences[seq_index], gpu_centroidAlignmentSums, nElementsForMean, pathMatrix[currDevice], centerLength, current_seq_length[currDevice], pathPitch[currDevice], flip_seq_order[currDevice]);
			CUERR("Launching kernel for centroid update");
		}

		// After all available kernel calls have been queued across available devices, wait for them all to finish before launching
		// the next set of calls (e.g. if 3 devices, but 14 sequences to process we will sync after queueing seq #3, then after #6, #9, #12, #14). 
		if(currDevice != deviceCount-1 && seq_index+1 < num_sequences){
			continue; // more cost-calculating DTW to queue up for parallel execution
		}

		/*** EVERYTHING BELOW HERE IS EFFECTIVELY CONDITIONALLY EXECUTED !! ***/
		int stripeCount = 0;
		for(int queuedDevice = 0; queuedDevice <= currDevice; queuedDevice++){
			if(usingStripePath[queuedDevice]){
				stripeCount++;
			}
		}

	        // If true, we must operate in "striped" path mode, which means we need to
        	// start in the upper right corner of the cost matrix and work our way backward to the lower left corner
        	// by successively recalculating the DTW costs and paths from the known left edge of a threadblock swath (which
        	// was stored in the cost matrix) to the right edge.
               	if(stripeCount > 0){ // This becomes blocking. No simple way around this without using tons more memory.
			size_t offset_within_seq[currDevice+1];
			size_t j_completed[currDevice+1]; // for striped path printing
			int remaining_offsets_to_process = 0;
			for(int queuedDevice = 0; queuedDevice <= currDevice; queuedDevice++){
				if(usingStripePath[queuedDevice]){
					offset_within_seq[queuedDevice] = flip_seq_order[queuedDevice] ? current_seq_length[queuedDevice] : centerLength;
					remaining_offsets_to_process += offset_within_seq[queuedDevice];
				}
			}
			while(remaining_offsets_to_process){ // There is at least one device that hasn't gotten to the left edge of the alignment cost matrix yet
				remaining_offsets_to_process = 0;
				for(int queuedDevice = 0; queuedDevice <= currDevice; queuedDevice++){
					if(!usingStripePath[queuedDevice]){
						continue;
					}
					dim3 threadblockDim(maxThreads[queuedDevice], 1, 1);
					cudaSetDevice(queuedDevice);
                			// We need to assign a path matrix big enough to handle the results of one vertical swath of the DTW calculation, so we can record the path steps
                			if(pathMatrix[queuedDevice] == 0){ // assign it only on the first rightmost stripe of the traceback and reuse (any subsequent leftward rounds will require the same or less)
                				// Gracefully degrade to manually pitched managed memory if this allocation fails.
						if(cudaMallocPitch(&pathMatrix[queuedDevice], &pathPitch[queuedDevice], threadblockDim.x*sizeof(unsigned char), cpu_backtrace_rows[queuedDevice])){  
							//std::cerr << "Stripe pitched managed memory for path matrix for device "<< queuedDevice<<std::endl;
							cudaMallocManaged(&pathMatrix[queuedDevice], sizeof(unsigned char)*pathPitch[queuedDevice]*cpu_backtrace_rows[queuedDevice]); CUERR("Allocating pseudo-pitched managed memory for striped step matrix");
							cudaStreamAttachMemAsync(seq_stream[queuedDevice], pathMatrix[queuedDevice]); CUERR("Attaching pseudo-pitched managed memory for striped step matrix to the corresponding sequence stream");
						}
						//std::cerr << "Stripe path matrix on host for device "<< queuedDevice<<": " << sizeof(unsigned char) << " x " << pathPitch[queuedDevice] << " x " << cpu_backtrace_rows[queuedDevice] << std::endl;
						// Using a standard malloc as this is GPU -> CPU read-once memory, no compelling need to burden the OS with massive page locked memory
						cpu_stepMatrix[queuedDevice] = (unsigned char *) std::malloc(sizeof(unsigned char)*pathPitch[queuedDevice]*cpu_backtrace_rows[queuedDevice]);
					       	if(cpu_stepMatrix[queuedDevice] == 0){
							std::cerr << "Allocating normal CPU memory for path matrix for striped sequence-centroid DTW path traceback" << std::endl;
							exit(CANNOT_ALLOCATE_HOST_STRIPED_STEP_MATRIX);
						}
					}
                			// We have a circular buffer in shared memory of three diagonals for minimal proper DTW calculation.
                			int shared_memory_required = threadblockDim.x*3*sizeof(T);
                			// Start the calculation at the last saved cost matrix column that's a multiple of the threadblock width by using integer math (i.e. the last vertical swath
                			// of the full cost matrix is the one to be recalculated for traceback, then the second last, etc. until we are at the left most column,
                			// which represents the start of the DTW alignment).
					if(offset_within_seq[queuedDevice] == 0){ // had nothing to process this round
                                                continue;
					}
					// Round down the offset to the next swath start (multiple of the threadblock width), specifically for the first round where it's likely a partial swath (rightmost)
					int left_column = (std::ceil(((float) offset_within_seq[queuedDevice])/threadblockDim.x)-1)*threadblockDim.x;
                        		// Index into the cost matrix is the LEFT edge of the swath being processed.
                        		T *existingCosts = dtwCostSoFar[queuedDevice] + (left_column-threadblockDim.x)/threadblockDim.x*(flip_seq_order[queuedDevice] ? centerLength : current_seq_length[queuedDevice]);
                        		if(flip_seq_order[queuedDevice]){
                                		// Specify both the first and second sequences explicitly (seq_index will actually be ignored).
                                		// Note that we aren't computing all the vertical swath necessarily, only up to where the DTW traceback has gotten (cpu_backtrace_rows[queuedDevice])
                                		// us so far in eating up the Y axis sequence (DP matrix row index i, but using i+1 since it's a "size" parameter, not an index).
                                		// This cuts the average number of alignment cost re-calculations in half for the striped traceback vs. a full matrix re-calc.
                                		DTWDistance<<<1,threadblockDim,shared_memory_required,seq_stream[queuedDevice]>>>(C, cpu_backtrace_rows[queuedDevice], sequences[seq_index-currDevice+queuedDevice], current_seq_length[queuedDevice],
                                               		PARAM_NOT_USED, left_column, (T *)PARAM_NOT_USED, PARAM_NOT_USED, PARAM_NOT_USED, (size_t *)PARAM_NOT_USED,
                                               		existingCosts, (T *)PARAM_NOT_USED,
                                               		pathMatrix[queuedDevice], pathPitch[queuedDevice], 
							(T *) PARAM_NOT_USED, use_open_start, use_open_end); CUERR("Flipped consensus DTW vertical swath calculation launch with path storage");
                        		}
                        		else{
                                		// Specify both the first and second sequences explicitly (seq_index will actually be ignored).
                                		DTWDistance<<<1,threadblockDim,shared_memory_required,seq_stream[queuedDevice]>>>(sequences[seq_index-currDevice+queuedDevice], cpu_backtrace_rows[queuedDevice], C, centerLength,
                                               		PARAM_NOT_USED, left_column, (T *)PARAM_NOT_USED, PARAM_NOT_USED, PARAM_NOT_USED, (size_t *) PARAM_NOT_USED,
                                               		existingCosts, (T *)PARAM_NOT_USED,
                                               		pathMatrix[queuedDevice], pathPitch[queuedDevice], 
							(T *) PARAM_NOT_USED, use_open_start, use_open_end); CUERR("Sequence DTW vertical swath calculation launch with path storage");
                        		}
					// The i matrix vertical index (saved as [cg]pu_backtrace_rows) will gradually decrease as we move from the top of the full alignment matrix to the bottom, but j (horizontal) is local to the stripe.
					int j = offset_within_seq[queuedDevice]%maxThreads[queuedDevice]; // was it a partial block filled in the path matrix that was allocated?
                        		if(j == 0){ // it was a full block, set the width accordingly (since a zero width block would never be run)
					       	j = maxThreads[queuedDevice]; 
					}
				    	// Update the amount processed to include the just finished stripe DTW
					offset_within_seq[queuedDevice] -= j;
					remaining_offsets_to_process += offset_within_seq[queuedDevice];
					// Partial centroid update, just for the path part inside the stripe we recalculated above. Will update value of gpu_backtrace_rows too with new height of remaining matrix to view.
					int pathColumns = flip_seq_order[queuedDevice] ? current_seq_length[queuedDevice] : j;
					int pathRows = flip_seq_order[queuedDevice] ? j : centerLength;
					updateCentroid<<<1,1,0,seq_stream[queuedDevice]>>>(sequences[seq_index-currDevice+queuedDevice], gpu_centroidAlignmentSums, nElementsForMean, pathMatrix[queuedDevice], 
							pathColumns, pathRows,
							pathPitch[queuedDevice], flip_seq_order[queuedDevice], offset_within_seq[queuedDevice], gpu_backtrace_rows[queuedDevice]);  CUERR("Launching centroid update using striped path");
					j_completed[queuedDevice] = j;
				}
				for(int queuedDevice = 0; queuedDevice <= currDevice; queuedDevice++){ // Print the partial paths serially
					cudaSetDevice(queuedDevice);
					if(!usingStripePath[queuedDevice]){
						continue;
					}
                       			cudaStreamSynchronize(seq_stream[queuedDevice]); // wait for the result of the DTW calculation for the swath/stripe
					//std::cerr << "Copying back pathMatrix to CPU for index " << offset_within_seq[queuedDevice] << ": " << sizeof(unsigned char)*pathPitch[queuedDevice]*cpu_backtrace_rows[queuedDevice] << std::endl;
                                	cudaMemcpy(cpu_stepMatrix[queuedDevice], pathMatrix[queuedDevice], 
							sizeof(unsigned char)*pathPitch[queuedDevice]*cpu_backtrace_rows[queuedDevice], cudaMemcpyDeviceToHost);  CUERR("Copying GPU to CPU memory for striped step matrix in DBA update");
#if DEBUG == 1
					/* Start of debugging code, which saves the DTW path for each sequence vs. consensus. Requires C++11 compatibility. */
					std::string step_filename = output_prefix+std::string("stepmatrix")+std::to_string(seq_index-currDevice+queuedDevice)+"."+std::to_string(offset_within_seq[queuedDevice]);
					writeDTWPathMatrix<T>(cpu_stepMatrix[queuedDevice], step_filename.c_str(), j_completed[queuedDevice], cpu_backtrace_rows[queuedDevice], pathPitch[queuedDevice]);
					//writeDTWPathMatrix<T>(pathMatrix[queuedDevice], step_filename.c_str(), j_completed[queuedDevice], cpu_backtrace_rows[queuedDevice], pathPitch[queuedDevice]);
#endif
					
                        		// Note this stripe's steps for the traceback, before we reuse the pathMatrix buffer for the left-neighbouring stripe.
					// Even if you don't want to print, you have to do this so that the next kernel launch of DTWDistance above has the updated value for cpu_backtrace_rows.
					writeDTWPath(cpu_stepMatrix[queuedDevice], cpu_backtrace_outputstream[queuedDevice], sequences[seq_index-currDevice+queuedDevice], 
					//writeDTWPath(pathMatrix[queuedDevice], cpu_backtrace_outputstream[queuedDevice], sequences[seq_index-currDevice+queuedDevice], 
							sequence_names[seq_index-currDevice+queuedDevice], current_seq_length[queuedDevice], 
							cpu_centroid, centerLength, j_completed[queuedDevice], 0, pathPitch[queuedDevice], flip_seq_order[queuedDevice], 
							offset_within_seq[queuedDevice], &cpu_backtrace_rows[queuedDevice]);
                		}
			} // end while(remaining_offsets_to_process)
		} // end if(stripeCount)

		for(int queuedDevice = 0; queuedDevice <= currDevice; queuedDevice++){
			cudaSetDevice(queuedDevice);
			cudaStreamSynchronize(seq_stream[queuedDevice]);  CUERR("Synchronizing prioritized CUDA stream in device-parallel update of sequence-centroid path calculations");
                	cudaFree(dtwCostSoFar[queuedDevice]); CUERR("Freeing DTW intermediate cost values in DBA cleanup");
			dtwCostSoFar[queuedDevice] = 0;
                	if(newDtwCostSoFar[queuedDevice] != 0){
				cudaFree(newDtwCostSoFar[queuedDevice]); CUERR("Freeing new DTW intermediate cost values in DBA cleanup");
				newDtwCostSoFar[queuedDevice] = 0;
			}
        		cudaStreamDestroy(seq_stream[queuedDevice]); CUERR("Removing a CUDA stream after completion of DBA cleanup");

			int num_columns = centerLength;
			int num_rows = current_seq_length[queuedDevice];
			if(flip_seq_order[queuedDevice]){int tmp = num_rows; num_rows = num_columns; num_columns = tmp;}
		
			if(!output_prefix.empty() && !usingStripePath[queuedDevice]){ // only works if you have the full path matrix available
	        		if((cpu_stepMatrix[queuedDevice] = (unsigned char *) std::malloc(sizeof(unsigned char)*pathPitch[queuedDevice]*num_rows)) == 0){
					std::cerr << "Cannot allocate standard CPU memory for full step matrix" << std::endl;
					exit(CANNOT_ALLOCATE_HOST_FULL_STEP_MATRIX);
				}
        			cudaMemcpy(cpu_stepMatrix[queuedDevice], pathMatrix[queuedDevice], sizeof(unsigned char)*pathPitch[queuedDevice]*num_rows, cudaMemcpyDeviceToHost);  CUERR("Copying GPU to CPU memory for step matrix in DBA update");

#if DEBUG == 1
				/* Start of debugging code, which saves the DTW path for each sequence vs. consensus. Requires C++11 compatibility. */
				std::string step_filename = output_prefix+std::string("stepmatrix")+std::to_string(seq_index-currDevice+queuedDevice);
				writeDTWPathMatrix<T>(cpu_stepMatrix[queuedDevice], step_filename.c_str(), num_columns, num_rows, pathPitch[queuedDevice]);
#endif
		
				writeDTWPath(cpu_stepMatrix[queuedDevice], cpu_backtrace_outputstream[queuedDevice], sequences[seq_index-currDevice+queuedDevice], 
						sequence_names[seq_index-currDevice+queuedDevice], current_seq_length[queuedDevice], cpu_centroid, 
						centerLength, num_columns, num_rows, pathPitch[queuedDevice], flip_seq_order[queuedDevice]);

			}
			(*(cpu_backtrace_outputstream[queuedDevice])).close();
			delete cpu_backtrace_outputstream[queuedDevice];
			if(cpu_stepMatrix[queuedDevice]){
				std::free(cpu_stepMatrix[queuedDevice]);
				cpu_stepMatrix[queuedDevice] = 0;
			}
                	if(pathMatrix[queuedDevice] != 0){ // skip if using striped mode
				cudaFree(pathMatrix[queuedDevice]); CUERR("Freeing DTW path matrix in DBA cleanup");
				pathMatrix[queuedDevice] = 0;
			}
		}

        }
	cudaFreeHost(maxThreads); CUERR("Freeing CPU memory for device thread properties");

	// Everything generated in the device-specific streams should be synced when we get here, so this is perfunctory. 
	// Multiple DBAs could be running on the same device and not interfere with each other at this step.
        cudaStreamSynchronize(stream); CUERR("Synchronizing master CUDA stream after all DBA update DTW calculations and centroid updates");

	cudaMemcpy(cpu_nElementsForMean, nElementsForMean, sizeof(unsigned int)*centerLength, cudaMemcpyDeviceToHost); CUERR("Copying barycenter update sequence pileup from GPU to CPU");
	cudaMemcpy(updatedMean, gpu_centroidAlignmentSums, sizeof(T)*centerLength, cudaMemcpyDeviceToHost); CUERR("Copying barycenter update sequence element sums from GPU to CPU");
	cudaStreamSynchronize(stream);  CUERR("Synchronizing CUDA stream before computing centroid mean");
	for (int t = 0; t < centerLength; t++) {
		updatedMean[t] /= cpu_nElementsForMean[t];
	}
	cudaFree(gpu_centroidAlignmentSums); CUERR("Freeing GPU memory for the barycenter update sequence element sums");
	cudaFree(nElementsForMean); CUERR("Freeing GPU memory for the barycenter update sequence pileup");
	cudaFreeHost(cpu_nElementsForMean);  CUERR("Freeing CPU memory for the barycenter update sequence pileup");

	// Calculate the difference between the old and new barycenter.
	// Convergence is defined as when all points in the old and new differ by less than a 
	// given delta (relative to std dev since every sequence is Z-normalized), so return the max point delta.
	double max_delta = (double) 0.0f;
	for(int t = 0; t < centerLength; t++) {
		double delta = std::abs((double) (cpu_centroid[t]-updatedMean[t]));
		if(delta > max_delta){
			max_delta = delta;
		}
	}
	cudaFreeHost(cpu_centroid); CUERR("Freeing CPU memory for the incoming centroid");

	delete[] dtwCostSoFar; // Play nice and clean up the dynamic heap allocations.
        delete[] newDtwCostSoFar;
        //delete[] gpu_backtrace_rows;
        //delete[] pathMatrix;
        delete[] cpu_backtrace_outputstream;
        delete[] cpu_stepMatrix;

	return max_delta;

}

/**
 * Performs the DBA averaging by first finding the median over a sample,
 * then doing iterations of the update until  the convergence condition is met.
 * 
 * @param sequences
 *                ragged 2D array of numeric sequences (of type T) to be averaged
 * @param num_sequences
 *                the number of sequences to be run through the algorithm
 * @param sequence_lengths
 *                the length of each member of the ragged array
 * @param algo_mode
 * 		  CLUSTER_ONLY, CONSENSUS_ONLY, or CLUSTER_AND_CONSENSUS
 */
template <typename T>
__host__ void performDBA(T **sequences, int num_sequences, size_t *sequence_lengths, char **sequence_names, int use_open_start, int use_open_end, char *output_prefix, int norm_sequences, double cdist, char** series_file_names, int num_series, int read_mode, bool is_segmented, int algo_mode, cudaStream_t stream=0) {

	//std::cerr << "Seq lengths" << std::endl;
	// Sanitize the data from potential upstream artifacts or overflow situations
	for(int i = 0; i < num_sequences; i++){
		if(sequences[i][sequence_lengths[i]-1] >= sqrt(std::numeric_limits<T>::max())){
			sequence_lengths[i]--; // truncate the sequence to get rid of the problematic value
		}
	}

	// Sort the sequences by length for memory efficiency in computation later on.
	//std::cerr << "Seq copy" << std::endl;
	size_t *sequence_lengths_copy;
	cudaMallocHost(&sequence_lengths_copy, sizeof(size_t)*num_sequences); CUERR("Allocating CPU memory for sortable copy of sequence lengths");
	if(memcpy(sequence_lengths_copy, sequence_lengths, sizeof(size_t)*num_sequences) != sequence_lengths_copy){
		std::cerr << "Running memcpy to populate sequence_lengths_copy failed" << std::endl;
		exit(MEMCPY_FAILURE);
	}
	thrust::sort_by_key(sequence_lengths_copy, sequence_lengths_copy + num_sequences, sequences); CUERR("Sorting sequences by length");
	thrust::sort_by_key(sequence_lengths, sequence_lengths + num_sequences, sequence_names); CUERR("Sorting sequence names by length");
	cudaFreeHost(sequence_lengths_copy); CUERR("Freeing CPU memory for sortable copy of sequence lengths");
	size_t maxLength = sequence_lengths[num_sequences-1];

	// Send the sequence metadata and data out to all the devices being used.
        int deviceCount;
        cudaGetDeviceCount(&deviceCount); CUERR("Getting GPU device count in DBA setup method");
	//std::cerr << "Dev count" << std::endl;
#if DEBUG == 1
        std::cerr << "Devices found: " << deviceCount << std::endl;
#endif

	// Z-normalize the sequences to match the in parallel on the GPU, once all the async memcpy calls are done.
	// Because this normalizatios in-place to save memory, and we are wanting to scale the averaged sequences back to their original range after the DBA calculations,
	// the most efficient thing to do is just store the mu and sigma values for all seqs so the medoids' specs can be restored after averaging without having 
	// kept a copy of the original data in memory.
	double *sequence_means;
	double *sequence_sigmas;
	//std::cerr << "Seq norm" << std::endl;
	if(norm_sequences){
		cudaMallocManaged(&sequence_means, sizeof(double)*num_sequences); CUERR("Allocating managed memory for array of sequence means");
		cudaMallocManaged(&sequence_sigmas, sizeof(double)*num_sequences); CUERR("Allocating managed memory for array of sequence sigmas");

#if DEBUG == 1
		std::cerr << "Normalizing " << num_sequences << " input streams (longest is " << maxLength << ")" << std::endl;
#endif
	       	normalizeSequences(sequences, num_sequences, sequence_lengths, -1, sequence_means, sequence_sigmas, stream);
	}

	int* sequences_membership = new int[num_sequences];
	int *medoidIndices;

	if(algo_mode == CLUSTER_AND_CONSENSUS || algo_mode == CLUSTER_ONLY){
		//std::cerr << "Clustering data" << std::endl;
		// Calculate the clusters
		T *gpu_sequences = 0;
		cudaMallocManaged(&gpu_sequences, sizeof(T)*num_sequences*maxLength); CUERR("Allocating GPU memory for array of evenly spaced sequences");
		// Make a GPU copy of the input ragged 2D array as an evenly spaced 1D array for performance (at some cost to space if very different lengths of input are used)
		for (int i = 0; i < num_sequences; i++) {
       			cudaMemcpyAsync(gpu_sequences+i*maxLength, sequences[i], sequence_lengths[i]*sizeof(T), cudaMemcpyHostToDevice, stream); CUERR("Copying sequence to GPU memory");
		}

		cudaStreamSynchronize(stream); CUERR("Synchronizing the CUDA stream after sequences' copy to GPU");
        	// Pick a seed sequence from the original input, with the smallest L2 norm (residual sum of squares).
		setupPercentageDisplay(CONCAT2("Step 2 of 3: Finding initial ",(cdist != 1 ? "clusters and medoids" : "medoid")));
		medoidIndices = approximateMedoidIndices(gpu_sequences, maxLength, num_sequences, sequence_lengths, sequence_names, use_open_start, use_open_end, output_prefix, 
	 		                                 &cdist, sequences_membership, stream);
		cudaFree(gpu_sequences); CUERR("Freeing CPU memory for GPU sequence data");
	}
	else if(algo_mode == CONSENSUS_ONLY){
		// Read from a previous call to this method.
		std::cerr << "Reading previous clustering data" << std::endl;
		medoidIndices = readMedoidIndices(CONCAT2(output_prefix, ".cluster_membership.txt").c_str(), num_sequences, sequence_names, sequences_membership);
	}
	else{
		std::cerr << "Call to performDBA included an unrecognized algorithm mode " << algo_mode << " (programming error, please contact the developer)" << std::endl;
                exit(UNKNOWN_ALGO);
	}
	teardownPercentageDisplay();	
	// Don't need the full complement of evenly space sequences again.

	int num_clusters = 1;
	for (int i = 0; i < num_sequences; i++) {
                if(sequences_membership[i] > num_clusters-1){
                        num_clusters = sequences_membership[i]+1;
                }
        }
	// No need to rewrite the (unchanged) membership file if we're in CONSENSUS_ONLY mode
	if(cdist != 1 && algo_mode != CONSENSUS_ONLY){ // in cluster mode
		std::ofstream membership_file(CONCAT2(output_prefix, ".cluster_membership.txt").c_str());
        	if(!membership_file.is_open()){
                	std::cerr << "Cannot open sequence cluster membership file " << CONCAT2(output_prefix, ".cluster_membership.txt").c_str() << " for writing" << std::endl;
                	exit(CANNOT_WRITE_MEMBERSHIP);
        	}
		membership_file << "## cluster distance threshold was " << cdist << std::endl;

		for (int i = 0; i < num_sequences; i++) {
			membership_file << sequence_names[i] << "\t" << sequences_membership[i] << "\t" << sequence_names[medoidIndices[sequences_membership[i]]] << std::endl;
		}
		membership_file.close();
		std::cerr << "Found " << num_clusters << " clusters using complete linkage and cluster distance cutoff " << cdist << std::endl;
	}
	// See if the caller's request was for just membership and act accordingly.
	if(algo_mode == CLUSTER_ONLY){
		return;
	}

	short **avgSequences = 0;
	char **avgNames = 0;
	size_t *avgSeqLengths = 0;
#if HDF5_SUPPORTED == 1 || SLOW5_SUPPORTED == 1
	
	cudaMallocHost(&avgSequences, sizeof(short*)*num_clusters);		 CUERR("Allocating GPU memory for average sequences");
	cudaMallocHost(&avgNames, sizeof(char*)*num_clusters);		 CUERR("Allocating GPU memory for average names");
	cudaMallocHost(&avgSeqLengths, sizeof(size_t)*num_clusters);		 CUERR("Allocating GPU average for medoid lengths");
#endif
	// To support checkpointing the compute, write each converged centroid as it's calculated, so we can pick up the computation after the last 
	// succesful cluster converged.
	int currCluster = 0;
	if(file_exists(CONCAT2(output_prefix, ".avg.txt").c_str())){
		currCluster = readSequenceAverages(CONCAT2(output_prefix, ".avg.txt").c_str(), avgSequences, avgNames, avgSeqLengths)+1;
		std::cerr << "Restarting convergence with cluster " << (currCluster+1) << "/" << num_clusters << " based on checkpoint in " << CONCAT2(output_prefix, ".avg.txt") << std::endl;
		// TODO: exit normally now if currCluster+1 == num_clusters?
	}
        std::ofstream avgs_file(CONCAT2(output_prefix, ".avg.txt").c_str(), std::ios::app);
        if(!avgs_file.is_open()){
                std::cerr << "Cannot open sequence averages file " << output_prefix << ".avg.txt for writing" << std::endl;
                exit(CANNOT_WRITE_DBA_AVG);
        }


	for(;currCluster < num_clusters; currCluster++){
		int num_members = 0;
		for (int i = 0; i < num_sequences; i++) {
                	if(sequences_membership[i] == currCluster){
                       		num_members++;
			}
                }

        	size_t medoidLength = sequence_lengths[medoidIndices[currCluster]];
		// Special case is when a cluster contains only one sequence, where we don't need to do anythiong except output the sequence as is.
		if(num_members == 1){
			std::cerr << "Outputting singleton sequence " << sequence_names[medoidIndices[currCluster]] << 
				     " as-is (a.k.a. cluster " << (currCluster+1) << "/" << num_clusters << ")." << std::endl;
#if HDF5_SUPPORTED == 1 || SLOW5_SUPPORTED == 1
			cudaMallocHost(&(avgSequences[currCluster]), sizeof(short)*medoidLength);		 CUERR("Allocating GPU memory for single average sequence");
#endif	
			avgs_file << sequence_names[medoidIndices[currCluster]];
			T *seq = sequences[medoidIndices[currCluster]];
			if(norm_sequences) {
                        	/* Rescale to ~original range (may have some floating point precision loss). */
                        	double seqAvg = sequence_means[medoidIndices[currCluster]];
                        	double seqStdDev = sequence_sigmas[medoidIndices[currCluster]];
        			for (size_t i = 0; i < medoidLength; ++i) { 
                                        avgs_file << "\t" << ((T) (seqAvg+seq[i]*seqStdDev));
#if HDF5_SUPPORTED == 1 || SLOW5_SUPPORTED == 1				
					avgSequences[currCluster][i] = (short)(seqAvg+seq[i]*seqStdDev);
#endif				
				}
			}
			else{
				for (size_t i = 0; i < medoidLength; ++i) {
                                        avgs_file << "\t" << seq[i];
#if HDF5_SUPPORTED == 1 || SLOW5_SUPPORTED == 1			
					avgSequences[currCluster][i] = (short)(seq[i]);
#endif				
				}
			}
			avgs_file << std::endl;
			avgs_file.flush(); // for checkpointing
			
#if HDF5_SUPPORTED == 1 || SLOW5_SUPPORTED == 1
			// Populate average buffers for writing fast5 output
			avgNames[currCluster] = sequence_names[medoidIndices[currCluster]];
			avgSeqLengths[currCluster] = medoidLength;
#endif
			
			continue;
		}

		T *gpu_barycenter = 0;
		cudaMallocManaged(&gpu_barycenter, sizeof(T)*medoidLength); CUERR("Allocating managed GPU memory for DBA result");
		// See if a partially-converged centroid already exists for this cluster (i.e. we should be picking up from a checkpoint)
		if(!readCentroidCheckpointFromFile(CONCAT4(output_prefix, ".", std::to_string(currCluster), ".evolving_centroid.txt").c_str(), gpu_barycenter, medoidLength)){
        		cudaMemcpyAsync(gpu_barycenter, sequences[medoidIndices[currCluster]], medoidLength*sizeof(T), cudaMemcpyDeviceToDevice, stream);  CUERR("Launching async copy of medoid seed to GPU memory");
		}

        	// Refine the alignment iteratively.
		T *new_barycenter = 0, *previous_barycenter, *two_previous_barycenter;
		cudaMallocHost(&new_barycenter, sizeof(T)*medoidLength); CUERR("Allocating CPU memory for DBA update result");
		if(use_open_start || use_open_end){
			cudaMallocHost(&previous_barycenter, sizeof(T)*medoidLength); CUERR("Allocating CPU memory for previous DBA update result");
			cudaMallocHost(&two_previous_barycenter, sizeof(T)*medoidLength); CUERR("Allocating CPU memory for two-back DBA update result");
		}

		std::cerr << "Processing cluster " << (currCluster+1) << " of " << num_clusters << ", " << 
			  num_members << " members, medoid " << sequence_names[medoidIndices[currCluster]] << " has length " << medoidLength << std::endl;
		// Allocate storage for an array of pointers to just the sequences from this cluster, so we generate averages for each cluster independently
		T **cluster_sequences;
		cudaMallocManaged(&cluster_sequences, sizeof(T**)*num_members); CUERR("Allocating GPU memory for array of cluster member sequence pointers");
		char **cluster_sequence_names;
		cudaMallocManaged(&cluster_sequence_names, sizeof(char**)*num_members); CUERR("Allocating GPU memory for array of cluster member sequence name pointers");
		size_t *member_lengths;
		cudaMallocManaged(&member_lengths, sizeof(T*)*num_members); CUERR("Allocating GPU memory for array of cluster member sequence pointers");

		num_members = 0;
		for (int i = 0; i < num_sequences; i++) {
                        if(sequences_membership[i] == currCluster){
				cluster_sequences[num_members] = sequences[i];
				cluster_sequence_names[num_members] = sequence_names[i];
				member_lengths[num_members] = sequence_lengths[i];
                                num_members++;
                        }
                }

#if DEBUG == 1
		int maxRounds = 1;
#else
		int maxRounds = 250; 
#endif
		cudaSetDevice(0);
		for (int i = 0; i < maxRounds; i++) {
			setupPercentageDisplay("Step 3 of 3 (round " + std::to_string(i+1) +  " of max " + std::to_string(maxRounds) + 
				       " to achieve delta 0) for cluster " + std::to_string(currCluster+1) + "/" + std::to_string(num_clusters) + ": Converging centroid");
			double delta = DBAUpdate(gpu_barycenter, medoidLength, cluster_sequences, cluster_sequence_names, num_members, member_lengths, use_open_start, use_open_end, 
					         new_barycenter, CONCAT3(output_prefix, ".", std::to_string(currCluster)), stream);
			teardownPercentageDisplay();
			std::cerr << "New delta is " << delta << std::endl;
			if(delta == 0){
				break; // converged!
			}
			// In open end mode (unlike global), it's possible for the centroid to flip between two nearly identical
			// centroids in perpetuity, so never really "converging". Handle this case with a shortcircuit.
			if(use_open_start || use_open_end){
				if(i >= 1 && !memcmp(new_barycenter, two_previous_barycenter, sizeof(T)*medoidLength)){
					std::cerr << "Detected a flip-flop between two alternative converged centroids (should happen only in open end mode), keeping the first one calculated" << std::endl;
					break;
				}
				if(i > 1){
					cudaMemcpy(two_previous_barycenter, previous_barycenter, medoidLength*sizeof(T), cudaMemcpyHostToHost); CUERR("Replacing two-back updated DBA medoid on host");
				}
				if(i > 0){
					cudaMemcpy(previous_barycenter, new_barycenter, medoidLength*sizeof(T), cudaMemcpyHostToHost); CUERR("Replacing previously updated DBA medoid on host");
				}
			}
			writeCentroidCheckpointToFile(CONCAT4(output_prefix, ".", std::to_string(currCluster), ".evolving_centroid.txt").c_str(), new_barycenter, medoidLength);
			cudaMemcpy(gpu_barycenter, new_barycenter, sizeof(T)*medoidLength, cudaMemcpyHostToDevice);  CUERR("Copying updated DBA medoid to GPU");
		}
		// Clean up the GPU memory we don't need any more.
		cudaFree(cluster_sequences); CUERR("Freeing GPU memory for array of cluster member sequence pointers");
		cudaFree(cluster_sequence_names); CUERR("Freeing GPU memory for array of cluster member sequence name pointers");
		cudaFree(member_lengths); CUERR("Freeing GPU memory for array of cluster member lengths");
		cudaFree(gpu_barycenter); CUERR("Freeing GPU memory for barycenter");

		if(norm_sequences) {
			/* Rescale the average to the centroid's value range. */
			double medoidAvg = sequence_means[medoidIndices[currCluster]];
 			double medoidStdDev = sequence_sigmas[medoidIndices[currCluster]];
			//std::cout << "Rescaling centroid to medoid's mean and std dev: " << medoidAvg << ", " << medoidStdDev << std::endl;
			for(int i = 0; i < medoidLength; i++){
				new_barycenter[i] = (T) (medoidAvg+new_barycenter[i]*medoidStdDev);
			}
		}
		avgs_file << sequence_names[medoidIndices[currCluster]];
        	for (size_t i = 0; i < medoidLength; ++i) { 
			avgs_file << "\t" << ((T *) new_barycenter)[i]; 
		}
		avgs_file << std::endl;
		avgs_file.flush(); // for checkpointing
		deleteCentroidCheckpointFile(CONCAT4(output_prefix, ".", std::to_string(currCluster), ".evolving_centroid.txt").c_str());
		
#if HDF5_SUPPORTED == 1 || SLOW5_SUPPORTED == 1
		// Populate medoid buffers for writing fast5 output
		avgNames[currCluster] = sequence_names[medoidIndices[currCluster]];
		avgSeqLengths[currCluster] = medoidLength;
		avgSequences[currCluster] = templateToShort(new_barycenter, avgSeqLengths[currCluster]);
#endif
		
		cudaFreeHost(new_barycenter); CUERR("Allocating CPU memory for DBA update result");
		if(use_open_start || use_open_end){
			cudaFreeHost(previous_barycenter); CUERR("Allocating CPU memory for previous DBA update result");
			cudaFreeHost(two_previous_barycenter); CUERR("Allocating CPU memory for two back DBA update result");
		}
	}
	
	if(norm_sequences){
		cudaFree(sequence_means);
		cudaFree(sequence_sigmas);
	}
        avgs_file.close();
	
#if HDF5_SUPPORTED == 1
	if(!is_segmented && read_mode == FAST5_READ_MODE && num_series == 1){
		std::cerr << "Writing medoids to new fast5 file..." << std::endl;
		if(writeFast5Output(series_file_names[0], CONCAT2(output_prefix, ".avg.fast5").c_str(), avgNames, avgSequences, avgSeqLengths, num_clusters) == 1){
			std::cerr << "Cannot write updated sequences to new Fast5 file " << CONCAT2(output_prefix, ".avg.fast5").c_str() << ", aborting." << std::endl;
			exit(CANNOT_WRITE_UPDATED_FAST5);
		}
		for(int i = 0; i < num_clusters; i++){
			cudaFreeHost(avgSequences[i]);      CUERR("Freeing GPU memory for an average sequence");
			cudaFreeHost(avgNames[i]);      CUERR("Freeing GPU memory for an average sequence name");
		}
		cudaFreeHost(avgSequences);	 CUERR("Freeing GPU memory for average sequence pointers");
		cudaFreeHost(avgNames);		 CUERR("Freeing GPU memory for average names");
		cudaFreeHost(avgSeqLengths);	 CUERR("Freeing GPU memory for average lengths");
	}		
#endif

#if SLOW5_SUPPORTED == 1
	if(!is_segmented && read_mode == SLOW5_READ_MODE && num_series == 1){
		std::cerr << "Writing medoids to new slow5 file..." << std::endl;
		if(writeSlow5Output(series_file_names[0], CONCAT2(output_prefix, ".avg.blow5").c_str(), avgNames, avgSequences, avgSeqLengths, num_clusters) == 1){
			std::cerr << "Cannot write updated sequences to new Fast5 file " << CONCAT2(output_prefix, ".avg.blow5").c_str() << ", aborting." << std::endl;
			exit(CANNOT_WRITE_UPDATED_SLOW5);
		}
		for(int i = 0; i < num_clusters; i++){
			cudaFreeHost(avgSequences[i]);      CUERR("Freeing GPU memory for an average sequence");
		}
		cudaFreeHost(avgSequences);	 CUERR("Freeing GPU memory for average sequence pointers");
		cudaFreeHost(avgNames);		 CUERR("Freeing GPU memory for average names");
		cudaFreeHost(avgSeqLengths);	 CUERR("Freeing GPU memory for average lengths");
	}		
#endif

	delete[] medoidIndices;
	delete[] sequences_membership;
}

/* Note that this method may adjust the total number of sequences, so that zero length sequences (after prefix chopping) do not go into the DBA later on. */
template <typename T>
__host__ void chopPrefixFromSequences(T *sequence_prefix, size_t sequence_prefix_length, T **sequences, int *num_sequences, size_t *sequence_lengths, char **sequence_names, char *output_prefix, int norm_sequences, cudaStream_t stream=0){

        // Send the sequence metadata and data out to all the devices being used.
        int deviceCount;
        cudaGetDeviceCount(&deviceCount); CUERR("Getting GPU device count in prefix chop method");
	int dotsPrinted = 0;

        T **gpu_sequence_prefixs = 0; // Using device side rather than managed to avoid potential memory page thrashing
        cudaMallocHost(&gpu_sequence_prefixs, sizeof(T*)*deviceCount); CUERR("Allocating CPU memory for array of device-side sequence prefix pointers");
        for(int currDevice = 0; currDevice < deviceCount; currDevice++){
                cudaSetDevice(currDevice);
                cudaMalloc(&gpu_sequence_prefixs[currDevice], sizeof(T)*sequence_prefix_length); CUERR("Allocating GPU memory for sequence prefix array member");
                cudaMemcpyAsync(gpu_sequence_prefixs[currDevice], sequence_prefix, sizeof(T)*sequence_prefix_length, cudaMemcpyHostToDevice, stream); CUERR("Copying sequence prefix to GPU memory for prefix chopping");
	}
	for(int i = 0; i < deviceCount; i++){
                cudaSetDevice(i);
                cudaDeviceSynchronize(); CUERR("Synchronizing CUDA device after sequence copy to GPU for chopping");
    		if(norm_sequences){
			normalizeSequence(gpu_sequence_prefixs[i], sequence_prefix_length, stream); CUERR("Normalizing sequence prefix for chopping");
		}
        }
    	if(norm_sequences){
	       	normalizeSequences(sequences, *num_sequences, sequence_lengths, -1, stream); CUERR("Normalizing input sequences for prefix chopping");
	}
	size_t *chopPositions = 0;
	cudaMallocHost(&chopPositions, sizeof(size_t)*(*num_sequences)); CUERR("Allocating CPU memory for sequence prefix chopping locations");

        unsigned int *maxThreads = getMaxThreadsPerDevice(deviceCount);
	// For testing purposes, see if 1024 is faster than maxThreads
	for(int i = 0; i < deviceCount; i++){
		maxThreads[i] = 1024;
	}

        // Declared sentinels to add semantics to DTWDistance call params.
        // A lot of DTW kernel parameters are ignored because we are launching without a real grid, so vars to infer 
        // kernel instance job divisions and result locations are not needed like they are in the medoid finding.
     	int DONT_USE_OPEN_START = 0; 
        int USE_OPEN_END = 1;
	int IGNORED_SEQ_INDEX_FROM_GRID = 0;
	size_t *IGNORED_GPU_SEQ_LENGTHS = 0;
	int IGNORED_GPU_SEQ_MAX_LENGTH = 0;
	T *IGNORED_SEQ_PTRS = 0;
	int IGNORED_NUM_SEQS = 0;
	T *NO_FINAL_COST_PAIR_MATRIX = 0;
        cudaStream_t *seq_streams;
	cudaMallocHost(&seq_streams, sizeof(cudaStream_t)*deviceCount); CUERR("Allocating CPU memory for sequence processing streams");
        T **dtwCostSoFars = 0;
        T **newDtwCostSoFars = 0;
	cudaMallocHost(&dtwCostSoFars, sizeof(T *)*deviceCount); CUERR("Allocating CPU memory for GPU DTW cost memory pointers");
	cudaMallocHost(&newDtwCostSoFars, sizeof(T *)*deviceCount); CUERR("Allocating CPU memory for GPU new DTW cost memory pointers");
	unsigned char **pathMatrixs = 0;
	cudaMallocHost(&pathMatrixs, sizeof(unsigned char *)*deviceCount); CUERR("Allocating CPU memory for GPU DTW path matrix pointers");
	// Record how many hits there are to each position in the leader in each input sequence.
        int **leaderPathHistograms = 0;
	cudaMallocHost(&leaderPathHistograms, sizeof(int **)*(*num_sequences)); CUERR("Allocating CPU memory for leader path histogram pointers");
	for(int i = 0; i < *num_sequences; i++){	 
		cudaMallocHost(&leaderPathHistograms[i], sizeof(int)*sequence_prefix_length); CUERR("Allocating CPU memory for a leader path histogram");
	}
        for(size_t seq_swath_start = 0; seq_swath_start < *num_sequences; seq_swath_start += deviceCount){

		for(int currDevice = 0; currDevice < deviceCount; currDevice++){
			size_t seq_index = seq_swath_start + currDevice;
			if(seq_index >= *num_sequences){
				break;
			}
			cudaSetDevice(currDevice);
                	size_t current_seq_length = sequence_lengths[seq_index];
       			// Need to run an open end DTW to find where the end of the prefix is in the input sequence based on the path
			// TODO: parallelize within each GPU (see memory alloc note below).
			size_t pathPitch = ((current_seq_length/512)+1)*512; // Have to pitch ourselves as no managed API for this exists

                	size_t dtwCostSoFarSize = sizeof(T)*sequence_prefix_length;
                	// This is small potatoes, we're in real trouble if we can't allocate this.
                	cudaMalloc(&dtwCostSoFars[currDevice], dtwCostSoFarSize);  CUERR("Allocating GPU memory for prefix chopping DTW pairwise distance intermediate values");
                	cudaMalloc(&newDtwCostSoFars[currDevice], dtwCostSoFarSize);  CUERR("Allocating GPU memory for prefix chopping new DTW pairwise distance intermediate values");
                
                        cudaStreamCreate(&seq_streams[currDevice]);
                
			// This is the potentially big matrix if either the prefix or the sequences are long, hence why we are not parallelizing with GPU for the moment.
                	cudaMallocManaged(&pathMatrixs[currDevice], pathPitch*sequence_prefix_length*sizeof(unsigned char)); CUERR("Allocating pitched GPU memory for prefix:sequence path matrix for prefix chopping");

       			dim3 threadblockDim(maxThreads[currDevice], 1, 1);
			int shared_memory_required = threadblockDim.x*3*sizeof(T);
			for(size_t offset_within_seq = 0; offset_within_seq < current_seq_length; offset_within_seq += threadblockDim.x){
        			DTWDistance<<<1,threadblockDim,shared_memory_required,seq_streams[currDevice]>>>(gpu_sequence_prefixs[currDevice], sequence_prefix_length, 
												      	     sequences[seq_index], current_seq_length, 
													     IGNORED_SEQ_INDEX_FROM_GRID, offset_within_seq, 
													     IGNORED_SEQ_PTRS, IGNORED_GPU_SEQ_MAX_LENGTH,
                                                 							     IGNORED_NUM_SEQS, IGNORED_GPU_SEQ_LENGTHS, 
													     dtwCostSoFars[currDevice], 
													     newDtwCostSoFars[currDevice],
													     pathMatrixs[currDevice], pathPitch, NO_FINAL_COST_PAIR_MATRIX, 
											 	 	     DONT_USE_OPEN_START, USE_OPEN_END); 
				CUERR("Launching DTW match of sequences to the sequence prefix");
				cudaMemcpyAsync(dtwCostSoFars[currDevice], newDtwCostSoFars[currDevice], dtwCostSoFarSize, cudaMemcpyDeviceToDevice, seq_streams[currDevice]); CUERR("Copying DTW sequence prefix costs between kernel calls");
			}
			dotsPrinted = updatePercentageComplete(seq_index+1, *num_sequences, dotsPrinted);
		}
       	        for(int currDevice = 0; currDevice < deviceCount; currDevice++){
			size_t seq_index = seq_swath_start + currDevice;
			if(seq_index >= *num_sequences){
				break;
			}
			cudaSetDevice(currDevice); CUERR("Setting active device for DTW path matrix results");
                	cudaStreamSynchronize(seq_streams[currDevice]); CUERR("Synchronizing CUDA device after sequence prefix swath calculation");
			cudaStreamDestroy(seq_streams[currDevice]); CUERR("Destroying now-redundant CUDA device stream");
			cudaFree(dtwCostSoFars[currDevice]);
			cudaFree(newDtwCostSoFars[currDevice]);

       			// Need to run an open end DTW to find where the end of the prefix is in the input sequence based on the path
			// TODO: parallelize within each GPU (see memory alloc note below).
                	size_t current_seq_length = sequence_lengths[seq_index];
			size_t pathPitch = ((current_seq_length/512)+1)*512; // Have to pitch ourselves as no managed API for this exists

			unsigned char *cpu_pathMatrix = 0;
                	size_t columnLimit = current_seq_length - 1;
                	size_t rowLimit = sequence_prefix_length - 1;
                	cudaMallocHost(&cpu_pathMatrix, sizeof(unsigned char)*pathPitch*sequence_prefix_length); CUERR("Allocating host memory for prefix DTW path matrix copy");
                	cudaMemcpy(cpu_pathMatrix, pathMatrixs[currDevice], sizeof(unsigned char)*pathPitch*sequence_prefix_length, cudaMemcpyDeviceToHost); CUERR("Copying prefix DTW path matrix from device to host");
#if DEBUG == 1
			//writeDTWPathMatrix(pathMatrixs[currDevice], (std::string("prefixchop_costmatrix")+std::to_string(seq_index)).c_str(), columnLimit+1, rowLimit+1, pathPitch);
#endif
			cudaFree(pathMatrixs[currDevice]);

                	int moveI[] = { -1, -1, 0, -1, 0 };
                	int moveJ[] = { -1, -1, -1, 0, -1 };
                	int j = columnLimit;
                	int i = rowLimit;
                	unsigned char move = cpu_pathMatrix[pitchedCoord(j,i,pathPitch)];
                	while (move == OPEN_RIGHT) {
                        	i += moveI[move];
                        	j += moveJ[move];
                        	move = cpu_pathMatrix[pitchedCoord(j,i,pathPitch)];
                	}
			chopPositions[seq_index] = j;
			// Now record how many positions in the query correspond to each position in the leader.
			int *leaderPathHistogram = leaderPathHistograms[seq_index];
			leaderPathHistogram[i] = 1;
			while (move != NIL) {
                                i += moveI[move];
                                j += moveJ[move];
				leaderPathHistogram[i]++;
                                move = cpu_pathMatrix[pitchedCoord(j,i,pathPitch)];
                        }
                	cudaFreeHost(cpu_pathMatrix);
        	}
	}
	cudaFreeHost(maxThreads); CUERR("Freeing CPU memory for device thread properties");
	cudaFreeHost(seq_streams); CUERR("Freeing CPU memory for prefix chopping CUDA streams");
	cudaFreeHost(dtwCostSoFars); CUERR("Freeing CPU memory for prefix chopping DTW cost intermediate values");
	cudaFreeHost(pathMatrixs); CUERR("Freeing CPU memory for prefix chopping DTW path matrices");
        for(int currDevice = 0; currDevice < deviceCount; currDevice++){
                cudaSetDevice(currDevice);
		cudaFree(gpu_sequence_prefixs[currDevice]); CUERR("Freeing GPU memory for a chopping device sequence prefix");
	}
	cudaFreeHost(gpu_sequence_prefixs); CUERR("Freeing CPU memory for chopping sequence prefix pointers");

	// We're going to have to free the incoming sequences once we've chopped them down and made a new more compact copy.
	std::ofstream chop((std::string(output_prefix)+std::string(".prefix_chop.txt")).c_str());
	int num_zero_length_sequences_skipped = 0;
	for(int i = 0; i < *num_sequences; i++){
		chop << sequence_names[i] << "\t" << chopPositions[i+num_zero_length_sequences_skipped] << "\t" << sequence_lengths[i];
		int *leaderPathHistogram = leaderPathHistograms[i+num_zero_length_sequences_skipped];
		for(int j = 0; j < sequence_prefix_length; j++){
			chop << "\t" << leaderPathHistogram[j];
		}
		chop << std::endl;

		size_t chopped_seq_length = sequence_lengths[i] - chopPositions[i+num_zero_length_sequences_skipped];

		// Remove from the inputs entirely as there is nothing left.
		if(chopped_seq_length == 0){
			std::cerr << "Skipping " << sequence_names[i] << " due to zero-length after prefix chopping" << std::endl;
			cudaFreeHost(leaderPathHistograms[i+num_zero_length_sequences_skipped]); CUERR("Freeing a leader path histogram array on host for zer-length sequence after prefix chop");
			num_zero_length_sequences_skipped++;
			for(int j = i+1; j < *num_sequences; j++){
				sequence_names[j-1] = sequence_names[j];
				sequences[j-1] = sequences[j];
				sequence_lengths[j-1] = sequence_lengths[j];
			}
			(*num_sequences)--;
			i--;
			continue;
		}
		T *new_seq = 0;
		cudaMallocManaged(&new_seq, sizeof(T)*chopped_seq_length); CUERR("Allocating host memory for chopped sequence pointers");
		T *chopped_seq_start = sequences[i]+chopPositions[i+num_zero_length_sequences_skipped];
		if(memcpy(new_seq, chopped_seq_start, sizeof(T)*chopped_seq_length) != new_seq){
                	std::cerr << "Running memcpy to copy prefix chopped sequence failed";
                	exit(CANNOT_COPY_PREFIX_CHOPPED_SEQ);
        	}
		cudaFree(sequences[i]); CUERR("Freeing managed sequence on host after prefix chop");
		sequences[i] = new_seq;
		sequence_lengths[i] = chopped_seq_length;
		cudaFreeHost(leaderPathHistograms[i+num_zero_length_sequences_skipped]); CUERR("Freeing a leader path histogram array on host");
	}
	chop.close();

	// TODO: normalize the signal based on the leader match

	cudaFreeHost(chopPositions); CUERR("Freeing chop position records on host");
	cudaFreeHost(leaderPathHistograms); CUERR("Freeing leader path histogram pointer array on host");
}

#endif
