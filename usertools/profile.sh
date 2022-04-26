#!/bin/bash

# profile the cost model for installed SSDs

# Precondition
# sudo ./perf -t 120 -o 131072 -q 16 -w write -c 1 -p 1 -r 'trtype:PCIe traddr:0000:00:1e.0'


CORE=0x01
DURATION=10
echo -e "RW\tBSIZE\tIOPS\tActual\tP50\tP90\tP95\tP99\tMax" > results.data

for RW_RATIO in 100 # 99 95 90 85 80 75 70 60 50 40 30 20 10 0
do
    for BLOCK in 4096 # 1024 8192 16384 32768 65536
    do
        for IOPS in 1000 10000 # 25000 50000 75000 100000 125000 150000
        do
            echo $RW_RATIO $BLOCK $IOPS >> results.data
            sudo ./perf -q 1024 -o $BLOCK -O $IOPS      \
                -w randrw -M $RW_RATIO                  \
                -t $DURATION -c $CORE -L                \
                -r 'trtype:PCIe traddr:0000:00:1e.0'    \
                | grep "Tail Latency" | cut -d':' -f 2 >> results.data
        done
    done
done
