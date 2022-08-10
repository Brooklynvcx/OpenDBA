PROGNAME=openDBA
#CXX=nvcc
# By default enable double precision float point number support (most NVIDIA GPU cards after 2016)
DOUBLE_UNSUPPORTED=0
# By default do not require HDF5 to compile (enable to allow reading Oxford Nanopore Technologies FAST5 files)
HDF5_SUPPORTED=0
# by default do not require SLOW5 to compile (alternative file format for Nanopore data)
SLOW5_SUPPORTED=0
DEBUG=0
# For kernel-side sqrt() support and getDeviceCount() calls respectively
NVCC_FLAGS+= --expt-relaxed-constexpr -rdc=true -maxrregcount 26 --std=c++11

ifeq ($(DEBUG),1)
  NVCC_FLAGS+= -g -G
endif

ifeq ($(HDF5_SUPPORTED),1)
  NVCC_FLAGS+= -lhdf5
endif

ifeq ($(SLOW5_SUPPORTED),1)
	LIBS += submodules/slow5lib/lib/libslow5.a -lz
endif

ifeq ($(DOUBLE_UNSUPPORTED),0)
  NVCC_FLAGS+= -arch=sm_61
endif

all: $(PROGNAME)

clean:
	rm -f openDBA.o multithreading.o submodules/hclust-cpp/fastcluster.o vendor/plugins/vbz_compression/build/bin/libvbz_hdf_plugin.so submodule/slow5lib/lib/libslow5.a tests/openDBA_test.o tests/io_utils_test $(PROGNAME)

# Following two targets are small external libraries with more less restrictive licenses (see headers for license info)
multithreading.o: multithreading.cpp
	nvcc -c $< -o $@

submodules/hclust-cpp/fastcluster.o: submodules/hclust-cpp/fastcluster.cpp submodules/hclust-cpp/fastcluster.h
	nvcc --compiler-options -lstdc++ -c submodules/hclust-cpp/fastcluster.cpp -o $@ 

openDBA.o: openDBA.cu openDBA.cuh clustering.cuh segmentation.hpp cpu_utils.hpp gpu_utils.hpp io_utils.hpp exit_codes.hpp read_mode_codes.h dtw.hpp dba.hpp limits.hpp cuda_utils.hpp
	nvcc -DCUB_IGNORE_DEPRECATED_CPP_DIALECT -DDEBUG=$(DEBUG) -DDOUBLE_UNSUPPORTED=$(DOUBLE_UNSUPPORTED) -DSLOW5_SUPPORTED=$(SLOW5_SUPPORTED)  -DHDF5_SUPPORTED=$(HDF5_SUPPORTED) $(NVCC_FLAGS) -c $< -o $@

plugins: vendor/plugins/vbz_compression/build/bin/libvbz_hdf_plugin.so

tests/openDBA_test.o: tests/openDBA_test.cu openDBA.cuh segmentation.hpp cpu_utils.hpp gpu_utils.hpp io_utils.hpp exit_codes.hpp read_mode_codes.h dtw.hpp dba.hpp limits.hpp cuda_utils.hpp
	nvcc -DCUB_IGNORE_DEPRECATED_CPP_DIALECT -DDEBUG=$(DEBUG) -DDOUBLE_UNSUPPORTED=$(DOUBLE_UNSUPPORTED) -DSLOW5_SUPPORTED=$(SLOW5_SUPPORTED)  -DHDF5_SUPPORTED=$(HDF5_SUPPORTED) $(NVCC_FLAGS) -c $< -o $@

tests/openDBA_test: tests/openDBA_test.cu tests/openDBA_test.o multithreading.o submodules/hclust-cpp/fastcluster.o $(LIBS) 
	nvcc $(NVCC_FLAGS) --compiler-options "-fPIC -no-pie" tests/openDBA_test.o multithreading.o submodules/hclust-cpp/fastcluster.o $(LIBS) -o tests/openDBA_test
	
tests/io_utils_test: tests/io_utils_test.cu io_utils.hpp cpu_utils.hpp multithreading.o $(LIBS) 
	nvcc -DDEBUG=$(DEBUG) -DDOUBLE_UNSUPPORTED=$(DOUBLE_UNSUPPORTED) -DSLOW5_SUPPORTED=$(SLOW5_SUPPORTED)  -DHDF5_SUPPORTED=$(HDF5_SUPPORTED) $(NVCC_FLAGS) --compiler-options "-fPIC -no-pie" $< multithreading.o $(LIBS) -o $@

tests: tests/openDBA_test tests/io_utils_test
	cd tests; ./openDBA_test ; ./io_utils_test

vendor/plugins/vbz_compression/build/bin/libvbz_hdf_plugin.so:
	git submodule update --init --recursive ;\
	mkdir -p vendor/plugins/vbz_compression/build ;\
	cd vendor/plugins/vbz_compression/build ;\
	cmake -D CMAKE_BUILD_TYPE=Release -D ENABLE_CONAN=OFF -D ENABLE_PERF_TESTING=OFF -D ENABLE_PYTHON=OFF .. ;\
	make
	
submodules/slow5lib/lib/libslow5.a:
	$(MAKE) -C submodules/slow5lib lib/libslow5.a
	
$(PROGNAME): Makefile openDBA.o multithreading.o submodules/hclust-cpp/fastcluster.o $(LIBS) 
	nvcc $(NVCC_FLAGS) --compiler-options -fPIC openDBA.o multithreading.o submodules/hclust-cpp/fastcluster.o $(LIBS) -o $(PROGNAME)

