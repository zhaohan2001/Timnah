cmake_minimum_required(VERSION 3.10)

# Find all cpp files.
file(GLOB_RECURSE SRC_FILES "*.cpp")
add_library(utils SHARED ${SRC_FILES}) 