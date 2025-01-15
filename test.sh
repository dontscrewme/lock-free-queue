#!/bin/bash

if [ $# -ne 2 ]; then
    echo "Usage: $0 <program> <number_of_times>"
    exit 1
fi

program=$1
count=$2

if ! [ "$count" -eq "$count" ] 2>/dev/null; then
    echo "Error: Second argument must be a number"
    exit 1
fi

for ((i=1; i<=$count; i++)); do
    echo "=== Execution #$i ==="
    output=$($program 2>&1)  # Capture both stdout and stderr

    echo "$output"
    if echo "$output" | grep -qi "FAILED"; then
        break;
    elif echo "$output" | grep -qi "WARNING"; then
        break;
    fi
    echo
done