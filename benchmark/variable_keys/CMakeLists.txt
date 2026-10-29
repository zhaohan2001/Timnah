cmake_minimum_required(VERSION 3.22)
project(Hashing1VariableKeys LANGUAGES CXX)

# This is an independent project: no directory-level link settings from the
# ordinary build or its index libraries enter this executable.
if(NOT HASHING_REPOSITORY)
  get_filename_component(HASHING_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
get_filename_component(HASHING_REPOSITORY "${HASHING_REPOSITORY}" REALPATH)
if(NOT EXISTS "${HASHING_REPOSITORY}/utils/common.hpp")
  message(FATAL_ERROR "HASHING_REPOSITORY must identify the Hashing1 source tree")
endif()
if(CMAKE_BINARY_DIR STREQUAL "${HASHING_REPOSITORY}/build")
  message(FATAL_ERROR "Use a separate variable-key build directory")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

find_package(fmt REQUIRED)
find_package(GTest REQUIRED)
find_package(gflags REQUIRED)
find_package(OpenMP REQUIRED)
find_package(jsoncpp REQUIRED)
find_package(Threads REQUIRED)
if(TARGET gflags::gflags)
  set(VARIABLE_GFLAGS_TARGET gflags::gflags)
else()
  set(VARIABLE_GFLAGS_TARGET gflags)
endif()

set(private_index "${HASHING_REPOSITORY}/hashindex/variable_keys")
set(private_sources
  "${private_index}/byte_index.cc"
  "${private_index}/chash/chash.cc"
  "${private_index}/dash/dash.cc"
  "${private_index}/ehcache/ehcache.cc"
  "${private_index}/timnah/timnah.cc"
  "${private_index}/timnah/storage.cc"
  "${private_index}/timnah/key_codec.cc"
  "${HASHING_REPOSITORY}/utils/numa_allocator.cpp")
add_library(variable_indexes STATIC ${private_sources})
target_compile_options(variable_indexes PRIVATE -O3 -march=native -fpic)
target_compile_definitions(variable_indexes PUBLIC CPU_NODE=0 CXL_NODE=2)
target_include_directories(variable_indexes BEFORE PUBLIC
  "${private_index}"
  "${private_index}/timnah"
  "${HASHING_REPOSITORY}"
  "${HASHING_REPOSITORY}/utils"
  "${HASHING_REPOSITORY}/benchmark/ycsb/modules")
target_link_libraries(variable_indexes PUBLIC fmt::fmt ${VARIABLE_GFLAGS_TARGET}
  GTest::GTest OpenMP::OpenMP_CXX jsoncpp Threads::Threads numa atomic)

add_executable(ycsb_variable
  main.cpp
  "${HASHING_REPOSITORY}/benchmark/ycsb/modules/parameter.cpp"
  "${HASHING_REPOSITORY}/benchmark/synthetic/dynamic_trace.cpp"
  "${HASHING_REPOSITORY}/benchmark/twitter/profiles.cpp"
  "${HASHING_REPOSITORY}/benchmark/webmail/profile.cpp")
set_target_properties(ycsb_variable PROPERTIES
  OUTPUT_NAME ycsb
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime")
target_compile_options(ycsb_variable PRIVATE -O3 -march=native -fpic)
target_link_libraries(ycsb_variable PRIVATE variable_indexes)

add_executable(variable_key_correctness "${private_index}/correctness.cpp")
target_compile_options(variable_key_correctness PRIVATE -O3 -march=native -fpic)
target_link_libraries(variable_key_correctness PRIVATE variable_indexes)

add_executable(variable_trace_correctness
  "${private_index}/trace_correctness.cpp"
  "${HASHING_REPOSITORY}/benchmark/ycsb/modules/parameter.cpp"
  "${HASHING_REPOSITORY}/benchmark/synthetic/dynamic_trace.cpp"
  "${HASHING_REPOSITORY}/benchmark/twitter/profiles.cpp"
  "${HASHING_REPOSITORY}/benchmark/webmail/profile.cpp")
target_compile_options(variable_trace_correctness PRIVATE -O3 -march=native -fpic)
target_link_libraries(variable_trace_correctness PRIVATE variable_indexes)

# The dispatcher starts the preserved scalar ELF or the independent byte-key
# executable before parsing benchmark flags or entering any measurement phase.
add_executable(ycsb_dispatch dispatch.cpp)
target_compile_options(ycsb_dispatch PRIVATE -O3)
if(NOT HASHING_SCALAR_SHA256)
  if(EXISTS "${HASHING_REPOSITORY}/build/benchmark/ycsb8/ycsb")
    file(SHA256 "${HASHING_REPOSITORY}/build/benchmark/ycsb8/ycsb" HASHING_SCALAR_SHA256)
  else()
    file(SHA256 "${HASHING_REPOSITORY}/build/benchmark/ycsb/ycsb" HASHING_SCALAR_SHA256)
  endif()
endif()
string(LENGTH "${HASHING_SCALAR_SHA256}" scalar_digest_length)
if(NOT scalar_digest_length EQUAL 64 OR NOT HASHING_SCALAR_SHA256 MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "HASHING_SCALAR_SHA256 must be a lowercase SHA256 digest")
endif()
target_compile_definitions(ycsb_dispatch PRIVATE
  HASHING_SCALAR_SHA256="${HASHING_SCALAR_SHA256}"
  HASHING_VARIABLE_EXECUTABLE="${CMAKE_CURRENT_BINARY_DIR}/runtime/ycsb")
# Keep the ordinary runtime dependency graph visible to existing collectors.
# These DSOs exist only in the short-lived dispatcher image; execv discards
# them before entering the independent variable executable.
target_link_options(ycsb_dispatch PRIVATE -Wl,--no-as-needed)
target_link_libraries(ycsb_dispatch PRIVATE
  "${HASHING_REPOSITORY}/build/hashindex/libhashindex.so"
  "${HASHING_REPOSITORY}/build/utils/libutils.so"
  fmt::fmt ${VARIABLE_GFLAGS_TARGET} OpenMP::OpenMP_CXX jsoncpp numa atomic)

add_custom_target(check_variable_keys
  COMMAND $<TARGET_FILE:variable_key_correctness>
  COMMAND $<TARGET_FILE:variable_trace_correctness>
  DEPENDS variable_key_correctness variable_trace_correctness
  USES_TERMINAL)
