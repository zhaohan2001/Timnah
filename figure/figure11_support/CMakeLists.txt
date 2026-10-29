cmake_minimum_required(VERSION 3.22)
project(Hashing1Figure11 LANGUAGES CXX)

if(NOT FIGURE11_REPOSITORY OR NOT FIGURE11_SOURCE_ROOT)
  message(FATAL_ERROR "Set FIGURE11_REPOSITORY and FIGURE11_SOURCE_ROOT")
endif()
if(NOT EXISTS "${FIGURE11_SOURCE_ROOT}/.figure11-diagnostic-copy.json")
  message(FATAL_ERROR "Run prepare.py before configuring the diagnostic build")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/driver.cpp")
  message(FATAL_ERROR "The Figure 11 driver.cpp is missing")
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

# Compile source COPIES directly into one executable. No production hashindex
# shared library is linked, overwritten, rebuilt or loaded by this target.
add_executable(figure11_probe
  driver.cpp
  "${FIGURE11_SOURCE_ROOT}/utils/numa_allocator.cpp"
  "${FIGURE11_SOURCE_ROOT}/hashindex/timnah/timnah.cc"
  "${FIGURE11_SOURCE_ROOT}/hashindex/timnah/storage.cc"
  "${FIGURE11_SOURCE_ROOT}/hashindex/timnah/key_codec.cc"
  "${FIGURE11_SOURCE_ROOT}/hashindex/dash/dash.cc"
  "${FIGURE11_SOURCE_ROOT}/hashindex/chash/chash.cc"
  "${FIGURE11_SOURCE_ROOT}/hashindex/ehcache/ehcache.cc"
  "${FIGURE11_SOURCE_ROOT}/benchmark/ycsb/modules/parameter.cpp"
  "${FIGURE11_SOURCE_ROOT}/benchmark/synthetic/dynamic_trace.cpp"
  "${FIGURE11_SOURCE_ROOT}/benchmark/twitter/profiles.cpp"
  "${FIGURE11_SOURCE_ROOT}/benchmark/webmail/profile.cpp")
target_compile_options(figure11_probe PRIVATE -march=native -fpic)
target_compile_definitions(figure11_probe PRIVATE CPU_NODE=0 CXL_NODE=2
  FIGURE11_DIAGNOSTIC_BUILD=1)
target_include_directories(figure11_probe BEFORE PRIVATE
  "${FIGURE11_SOURCE_ROOT}"
  "${FIGURE11_SOURCE_ROOT}/utils"
  "${FIGURE11_SOURCE_ROOT}/benchmark/ycsb/modules")
target_link_libraries(figure11_probe PRIVATE
  fmt::fmt gflags GTest::GTest GTest::Main OpenMP::OpenMP_CXX
  jsoncpp numa atomic)
