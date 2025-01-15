#!/bin/bash

# Check if the number of arguments is even
if [ $# -eq 0 ] || [ $(($# % 2)) -ne 0 ]; then
    echo "Usage: $0 <program1> <count1> <program2> <count2> ..."
    echo "Example: $0 './your_program' '2' './another_program' '4'"
    exit 1
fi

# Process pairs of arguments
while [ $# -gt 0 ]; do
    program=$1
    count=$2
    
    echo "Testing $program with $count attempts"
    echo "=================================="
    ./test.sh "$program" "$count"
    echo
    
    # Remove the processed pair from arguments
    shift 2
done