#!/bin/bash

# Capacity * 5 / BW
sudo ../spdk/build/examples/perf -q 128 -o 131072 -w write -t 120 -c 0x1 -r 'trtype:PCIe traddr:0000:00:1e.0'
sudo ../spdk/build/examples/perf -q 128 -o 4096 -w write -t 120 -c 0x1 -r 'trtype:PCIe traddr:0000:00:1e.0'
sudo ../spdk/build/examples/perf -q 128 -o 131072 -w randwrite -t 300 -c 0x1 -r 'trtype:PCIe traddr:0000:00:1e.0'
sudo ../spdk/build/examples/perf -q 128 -o 4096 -w randwrite -t 300 -c 0x1 -r 'trtype:PCIe traddr:0000:00:1e.0'
sudo ../spdk/build/examples/perf -q 128 -o 131072 -w randrw -M 50 -t 600 -c 0x1 -r 'trtype:PCIe traddr:0000:00:1e.0'
sudo ../spdk/build/examples/perf -q 128 -o 4096 -w randrw -M 50 -t 600 -c 0x1 -r 'trtype:PCIe traddr:0000:00:1e.0'
# verification
sudo ../spdk/build/examples/perf -q 128 -o 4096 -w randread -t 60 -c 0x1 -r 'trtype:PCIe traddr:0000:00:1e.0'
