cmake_minimum_required(VERSION 3.22)
project(Hashing1Figure12 LANGUAGES CXX)

if(NOT FIGURE12_REPOSITORY OR NOT FIGURE12_SOURCE_ROOT)
  message(FATAL_ERROR "Set FIGURE12_REPOSITORY and FIGURE12_SOURCE_ROOT")
endif()
if(NOT EXISTS "${FIGURE12_SOURCE_ROOT}/.figure12-diagnostic-copy.json")
  message(FATAL_ERROR "Run prepare.py before configuring the measurement build")
endif()
foreach(existing_library hashindex/libhashindex.so utils/libutils.so)
  if(NOT EXISTS "${FIGURE12_REPOSITORY}/build/${existing_library}")
    message(FATAL_ERROR "The unchanged production library is missing: ${existing_library}")
  endif()
endforeach()

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

# These are IMPORTED targets: this project has no rule that can build or modify
# production libraries. The existing factory selects the same index classes as
# the ordinary production ycsb executable.
add_library(figure12_production_factory SHARED IMPORTED)
set_target_properties(figure12_production_factory PROPERTIES
  IMPORTED_LOCATION "${FIGURE12_REPOSITORY}/build/hashindex/libhashindex.so")
add_library(figure12_production_utils SHARED IMPORTED)
set_target_properties(figure12_production_utils PROPERTIES
  IMPORTED_LOCATION "${FIGURE12_REPOSITORY}/build/utils/libutils.so")

add_executable(figure12_probe
  driver.cpp
  "${FIGURE12_SOURCE_ROOT}/benchmark/ycsb/modules/parameter.cpp"
  "${FIGURE12_SOURCE_ROOT}/benchmark/synthetic/dynamic_trace.cpp"
  "${FIGURE12_SOURCE_ROOT}/benchmark/twitter/profiles.cpp"
  "${FIGURE12_SOURCE_ROOT}/benchmark/webmail/profile.cpp")
target_compile_options(figure12_probe PRIVATE -march=native -fpic)
target_compile_definitions(figure12_probe PRIVATE CPU_NODE=0 CXL_NODE=2)
target_include_directories(figure12_probe BEFORE PRIVATE
  "${FIGURE12_SOURCE_ROOT}"
  "${FIGURE12_SOURCE_ROOT}/utils"
  "${FIGURE12_SOURCE_ROOT}/benchmark/ycsb/modules")
target_link_libraries(figure12_probe PRIVATE
  figure12_production_factory
  # Preserve the existing utility library in runtime provenance even when
  # optional index removal leaves no direct utility symbol reference.
  "-Wl,--push-state,--no-as-needed" figure12_production_utils "-Wl,--pop-state"
  fmt::fmt gflags GTest::GTest GTest::Main OpenMP::OpenMP_CXX
  jsoncpp numa atomic)
set_target_properties(figure12_probe PROPERTIES
  BUILD_RPATH "${FIGURE12_REPOSITORY}/build/hashindex;${FIGURE12_REPOSITORY}/build/utils")
