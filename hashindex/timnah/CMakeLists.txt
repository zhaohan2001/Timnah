add_library(timnah SHARED timnah.cc storage.cc key_codec.cc)
target_include_directories(timnah PUBLIC ${PROJECT_SOURCE_DIR})

add_executable(timnah_test timnah_test.cpp sampling_test.cpp)
target_link_libraries(timnah_test PRIVATE timnah)
# The project already owns an executable target named test.
add_custom_target(check_timnah
  COMMAND $<TARGET_FILE:timnah_test>
  DEPENDS timnah_test
  USES_TERMINAL)
