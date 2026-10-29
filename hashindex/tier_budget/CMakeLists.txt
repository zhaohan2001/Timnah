add_executable(tier_budget_test tiered_memory_test.cpp)
target_compile_features(tier_budget_test PRIVATE cxx_std_20)
add_custom_target(check_tier_budget
  COMMAND $<TARGET_FILE:tier_budget_test>
  DEPENDS tier_budget_test
  USES_TERMINAL)
