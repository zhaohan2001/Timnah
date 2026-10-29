#!/bin/bash
# Run the command in the first argument 100 times and append the results to the file in the second argument.

# Usage: ./correctness.sh [executable path] [result output path]

# Example: ./correctness.sh ./bin/Debug/Project1.exe ./test/correctness.txt

for i in {1..100}
do
    $1 >> $2
    # echo ${i}
done