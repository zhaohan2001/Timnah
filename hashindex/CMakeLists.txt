add_subdirectory(tier_budget)
# Enabled hash indexes; isolate CHash declarations to prevent include-path conflicts.
set(HASHINDEX chash dash timnah ehcache)
foreach(HASH ${HASHINDEX})
  add_subdirectory(${HASH})
endforeach()
add_library(hashindex SHARED hashindex.cpp)
target_link_libraries(hashindex ${HASHINDEX})
