cmake_minimum_required(VERSION 3.22)
project(Hashing1Figure14 LANGUAGES CXX)
if(NOT FIGURE14_REPOSITORY OR NOT FIGURE14_SOURCE_ROOT)
  message(FATAL_ERROR "Set FIGURE14_REPOSITORY and FIGURE14_SOURCE_ROOT")
endif()
if(NOT EXISTS "${FIGURE14_SOURCE_ROOT}/.figure14-diagnostic-copy.json")
  message(FATAL_ERROR "Run prepare.py before configuring the isolated build")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
find_package(fmt REQUIRED)
find_package(GTest REQUIRED)
find_package(gflags REQUIRED)
find_package(OpenMP REQUIRED)
find_package(jsoncpp REQUIRED)

set(timnah_dir "${FIGURE14_SOURCE_ROOT}/hashindex/timnah")
foreach(kind probe correctness)
  if(kind STREQUAL "probe")
    set(target figure14_probe)
    set(driver driver.cpp)
  else()
    set(target figure14_correctness)
    set(driver correctness.cpp)
  endif()
  add_executable(${target}
    ${driver}
    "${FIGURE14_SOURCE_ROOT}/utils/numa_allocator.cpp"
    "${timnah_dir}/timnah.cc"
    "${timnah_dir}/storage.cc"
    "${timnah_dir}/key_codec.cc"
    "${FIGURE14_SOURCE_ROOT}/hashindex/chash/chash.cc"
    "${FIGURE14_SOURCE_ROOT}/benchmark/ycsb/modules/parameter.cpp"
    "${FIGURE14_SOURCE_ROOT}/benchmark/synthetic/dynamic_trace.cpp"
    "${FIGURE14_SOURCE_ROOT}/benchmark/twitter/profiles.cpp"
    "${FIGURE14_SOURCE_ROOT}/benchmark/webmail/profile.cpp")
  target_compile_options(${target} PRIVATE -march=native -fpic)
  target_compile_definitions(${target} PRIVATE CPU_NODE=0 CXL_NODE=2)
  target_include_directories(${target} BEFORE PRIVATE
    "${timnah_dir}" "${FIGURE14_SOURCE_ROOT}" "${FIGURE14_SOURCE_ROOT}/utils"
    "${FIGURE14_SOURCE_ROOT}/benchmark/ycsb/modules")
  target_link_libraries(${target} PRIVATE fmt::fmt gflags GTest::GTest
    GTest::Main OpenMP::OpenMP_CXX jsoncpp numa atomic)
endforeach()
