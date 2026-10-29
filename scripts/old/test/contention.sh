
for num in 1 2 4 8 16 32 64;
do
numactl -N0 build/test/smallTablePerformance -preloadRecords=10000000 -testRecords=10000000 -numThread=${num};
done