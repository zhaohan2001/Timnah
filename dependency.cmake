# Find core libraries.
# ------------------------------------------------------------------------------

# Formatted output library: fmt
find_package(fmt REQUIRED)
link_libraries(fmt::fmt)

# Testing library: gtest
find_package(GTest REQUIRED)
link_libraries(GTest::GTest GTest::Main)

# Command-line parsing library: gflags
find_package(gflags REQUIRED)
link_libraries(gflags)

# Benchmark library: benchmark
find_package(benchmark REQUIRED)
link_libraries(benchmark)

# omp
find_package(OpenMP REQUIRED)
link_libraries(OpenMP::OpenMP_CXX)

# Import jsoncpp.
find_package(jsoncpp REQUIRED)
link_libraries(jsoncpp)

if(UNIX AND NOT APPLE)
  link_libraries(numa)
endif(UNIX AND NOT APPLE)

link_libraries(atomic)