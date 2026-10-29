for X in NH_C NH_S
do
/home/cwang/Playground/Hashing1/build/benchmark/ycsb/ycsb -index=$X -num_rounds=1 \
  -num_load_threads=1 -num_run_threads=56 -bind_core=false -verify=false -shuffle=false \
  -num_load=10000000 -num_run=100000000 -read_ratio=0.95 -update_ratio=0.05
echo "----Done with $X----"
done