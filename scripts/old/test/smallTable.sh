cd build;
for i in 13 14 15 16 17 18 19 20;
do
cmake -DSMALL=${i} ..;
ninja;
echo "small table size: ${i}";
numactl -N0 test/smallTablePerformance -preloadRecords=10000000 -testRecords=10000000 -numThread=1;
numactl -N0 test/smallTablePerformance -preloadRecords=10000000 -testRecords=10000000 -numThread=32;
done;