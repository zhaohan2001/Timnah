# Find all cpp files in this directory.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/modules)
file(GLOB_RECURSE CPP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/modules/*.cpp")

# Compile observed-workload replay and profile selection.
add_library(synthetic_workloads STATIC
  ../synthetic/dynamic_trace.cpp
  ../twitter/profiles.cpp
  ../webmail/profile.cpp)

# Add the discovered cpp files to the target.
add_executable(ycsb ycsb.cpp ${CPP_SOURCES})
add_executable(test test.cpp ${CPP_SOURCES})
target_link_libraries(ycsb PRIVATE synthetic_workloads)
target_link_libraries(test PRIVATE synthetic_workloads)

# Build each cpp file in module-test as a separate executable.
file(GLOB MODULE_TEST_FILES "module-test/*.cpp")
foreach(MODULE_TEST_FILE ${MODULE_TEST_FILES})
  get_filename_component(MODULE_TEST_NAME ${MODULE_TEST_FILE} NAME_WE)
  add_executable(${MODULE_TEST_NAME} ${MODULE_TEST_FILE})
endforeach(MODULE_TEST_FILE ${MODULE_TEST_FILES})

# Startup parameter and shared-trace checks, outside benchmark hot paths.
add_executable(workload_parameter_probe workload_tests/parameter_probe.cpp modules/parameter.cpp)
add_executable(ratio_trace_probe workload_tests/ratio_trace_probe.cpp modules/parameter.cpp)
target_link_libraries(workload_parameter_probe PRIVATE synthetic_workloads)
target_link_libraries(ratio_trace_probe PRIVATE synthetic_workloads)

add_executable(synthetic_trace_probe ../synthetic/dynamic_trace_probe.cpp)
target_link_libraries(synthetic_trace_probe PRIVATE synthetic_workloads)

# The standalone Dash correctness validator also compiles parameter.cpp.
# Keep its production library untouched; only satisfy its helper dependency.
if(TARGET dash_validate)
  target_link_libraries(dash_validate PRIVATE synthetic_workloads)
endif()

# Refresh an already installed optional dispatcher after an ordinary relink.
# Independent figure builds and checkouts without an installation are unchanged.
if(HASHING_VARIABLE_KEYS AND CMAKE_BINARY_DIR STREQUAL "${PROJECT_SOURCE_DIR}/build" AND EXISTS "${PROJECT_SOURCE_DIR}/benchmark/variable_keys/build.py")
  find_program(HASHING1_VARIABLE_PYTHON NAMES python3 REQUIRED)
  add_custom_command(TARGET ycsb POST_BUILD
    COMMAND "${HASHING1_VARIABLE_PYTHON}" "${PROJECT_SOURCE_DIR}/benchmark/variable_keys/build.py" --refresh-dispatch --repository "${PROJECT_SOURCE_DIR}"
    COMMENT "Refresh installed variable-key dispatch after an ordinary relink"
    VERBATIM)
endif()
