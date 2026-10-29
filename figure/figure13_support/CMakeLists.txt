cmake_minimum_required(VERSION 3.22)
project(Hashing1Figure13 LANGUAGES CXX)
if(NOT FIGURE13_REPOSITORY OR NOT FIGURE13_SOURCE_ROOT)
  message(FATAL_ERROR "Set FIGURE13_REPOSITORY and FIGURE13_SOURCE_ROOT")
endif()
if(NOT EXISTS "${FIGURE13_SOURCE_ROOT}/.figure13-diagnostic-copy.json")
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

set(variants eh tiered kv migration hot)
set(variant_id 0)
foreach(variant IN LISTS variants)
  if(variant STREQUAL "hot")
    set(timnah_dir "${FIGURE13_SOURCE_ROOT}/hashindex/timnah")
  else()
    set(timnah_dir "${FIGURE13_SOURCE_ROOT}/hashindex/timnah_ablation")
  endif()
  foreach(kind probe correctness)
    if(kind STREQUAL "probe")
      set(target "figure13_${variant}")
      set(driver driver.cpp)
    else()
      set(target "figure13_test_${variant}")
      set(driver correctness.cpp)
    endif()
    add_executable(${target}
      ${driver}
      "${FIGURE13_SOURCE_ROOT}/utils/numa_allocator.cpp"
      "${timnah_dir}/timnah.cc"
      "${timnah_dir}/storage.cc"
      "${timnah_dir}/key_codec.cc"
      "${FIGURE13_SOURCE_ROOT}/benchmark/ycsb/modules/parameter.cpp"
      "${FIGURE13_SOURCE_ROOT}/benchmark/synthetic/dynamic_trace.cpp"
      "${FIGURE13_SOURCE_ROOT}/benchmark/twitter/profiles.cpp"
      "${FIGURE13_SOURCE_ROOT}/benchmark/webmail/profile.cpp")
    target_compile_options(${target} PRIVATE -march=native -fpic)
    target_compile_definitions(${target} PRIVATE CPU_NODE=0 CXL_NODE=2
      FIGURE13_VARIANT=${variant_id} FIGURE13_VARIANT_ID="${variant}")
    target_include_directories(${target} BEFORE PRIVATE
      "${timnah_dir}" "${FIGURE13_SOURCE_ROOT}" "${FIGURE13_SOURCE_ROOT}/utils"
      "${FIGURE13_SOURCE_ROOT}/benchmark/ycsb/modules")
    target_link_libraries(${target} PRIVATE fmt::fmt gflags GTest::GTest
      GTest::Main OpenMP::OpenMP_CXX jsoncpp numa atomic)
  endforeach()
  math(EXPR variant_id "${variant_id}+1")
endforeach()
