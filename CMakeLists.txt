cmake_minimum_required(VERSION 3.22)
project(Hashing1)

# Use optimized builds by default while honoring an explicitly selected type.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
option(HASHING_VARIABLE_KEYS "Build and install the independent variable-key mode in the primary build" ON)

# Set compilation options.
# ------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)

if(LINUX)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif(LINUX)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native -fpic")

# Compiler flags
# ------------------------------------------------------------------------------
set(CPU_NODE 0)
set(CXL_NODE=2)

add_compile_definitions(CPU_NODE=${CPU_NODE})
add_compile_definitions(CXL_NODE=${CXL_NODE})

# Resolve library dependencies.
# ------------------------------------------------------------------------------
include(./dependency.cmake)

# Subproject include paths
# ------------------------------------------------------------------------------
include_directories(.) # Temporarily search for headers project-wide.
include_directories(utils)

add_subdirectory(utils)
link_libraries(utils)

# Build hash indexes.
# ------------------------------------------------------------------------------
add_subdirectory(hashindex)
link_libraries(hashindex)

# Build benchmarks.
# ------------------------------------------------------------------------------
add_subdirectory(benchmark)

# A complete primary build also builds and installs the independent byte-key
# executable. Source snapshots and alternate build directories stay isolated.
if(HASHING_VARIABLE_KEYS AND CMAKE_BINARY_DIR STREQUAL "${PROJECT_SOURCE_DIR}/build")
  if(NOT EXISTS "${PROJECT_SOURCE_DIR}/benchmark/variable_keys/build.py")
    message(FATAL_ERROR "Variable-key sources are missing; restore benchmark/variable_keys or set HASHING_VARIABLE_KEYS=OFF")
  endif()
  find_program(HASHING1_VARIABLE_PYTHON NAMES python3 REQUIRED)
  add_custom_target(variable_keys ALL
    COMMAND "${HASHING1_VARIABLE_PYTHON}" "${PROJECT_SOURCE_DIR}/benchmark/variable_keys/build.py"
      --repository "${PROJECT_SOURCE_DIR}"
      --build-directory "${PROJECT_SOURCE_DIR}/build/variable_keys"
    DEPENDS ycsb
    COMMENT "Build, test and install the independent variable-key mode"
    USES_TERMINAL
    VERBATIM)
endif()
