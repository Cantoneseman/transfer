cmake_minimum_required(VERSION 3.10)
project(HyperTransfer VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler flags for optimization
add_compile_options(-O3)

# Include directories
include_directories(include)

# Find LZ4 library
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LZ4 liblz4)
endif()

if(NOT LZ4_FOUND)
    find_library(LZ4_LIBRARY NAMES lz4 REQUIRED)
    set(LZ4_LIBRARIES ${LZ4_LIBRARY})
endif()

# Find xxHash library
find_library(XXHASH_LIBRARY NAMES xxhash REQUIRED)

# Find threading library
find_package(Threads REQUIRED)

# Define hyper_sender executable
add_executable(hyper_sender
    src/hyper_sender.cpp
    src/chunking.cpp
    src/compressor.cpp
)
target_link_libraries(hyper_sender PRIVATE 
    ${LZ4_LIBRARIES} 
    ${XXHASH_LIBRARY}
    Threads::Threads
)
if(LZ4_INCLUDE_DIRS)
    target_include_directories(hyper_sender PRIVATE ${LZ4_INCLUDE_DIRS})
endif()

# Define hyper_receiver executable
add_executable(hyper_receiver
    src/hyper_receiver.cpp
    src/compressor.cpp
)
target_link_libraries(hyper_receiver PRIVATE ${LZ4_LIBRARIES})
if(LZ4_INCLUDE_DIRS)
    target_include_directories(hyper_receiver PRIVATE ${LZ4_INCLUDE_DIRS})
endif()

# ==========================================
# TopologyManager Module
# ==========================================

# Compile topology library (shared by sender and tests)
add_library(topology_lib src/topology_manager.cpp)

# Compile topology test program
add_executable(test_topo src/test_topology.cpp)
target_link_libraries(test_topo topology_lib)

# Compile router CLI tool
add_executable(hyper_router src/hyper_router.cpp)
target_link_libraries(hyper_router topology_lib)