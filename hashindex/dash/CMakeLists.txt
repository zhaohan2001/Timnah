add_library(dash SHARED dash.cc)
target_include_directories(dash PUBLIC ${PROJECT_SOURCE_DIR})
add_executable(dash_test dash_test.cpp)
target_link_libraries(dash_test PRIVATE dash)
add_custom_target(check_dash
  COMMAND $<TARGET_FILE:dash_test>
  DEPENDS dash_test
  USES_TERMINAL)

# Independent exhaustive validator. It does not change the ycsb target or Dash's
# production lookup path. parameter.cpp supplies the real Trace generator flags.
add_executable(dash_validate
  dash_validate.cpp
  ${PROJECT_SOURCE_DIR}/benchmark/ycsb/modules/parameter.cpp)
target_link_libraries(dash_validate PRIVATE dash OpenMP::OpenMP_CXX)
