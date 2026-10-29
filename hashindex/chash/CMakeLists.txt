add_library(chash SHARED chash.cc)
target_include_directories(chash PUBLIC ${PROJECT_SOURCE_DIR})

add_executable(chash_test chash_test.cpp)
target_link_libraries(chash_test PRIVATE chash)
# The project already has a test executable, so do not enable CTest, which reserves that target name.
add_custom_target(check_chash
  COMMAND $<TARGET_FILE:chash_test>
  DEPENDS chash_test
  USES_TERMINAL)
