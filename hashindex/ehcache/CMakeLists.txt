add_library(ehcache SHARED ehcache.cc)
target_include_directories(ehcache PUBLIC ${PROJECT_SOURCE_DIR})
add_executable(ehcache_test ehcache_test.cpp)
target_link_libraries(ehcache_test PRIVATE ehcache)
add_custom_target(check_ehcache COMMAND $<TARGET_FILE:ehcache_test>
  DEPENDS ehcache_test USES_TERMINAL)
