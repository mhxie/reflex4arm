#!/bin/bash

# Capacity * 5 / BW
sudo ../spdk/build/examples/perf -q 128 -o 4096 -w write -t 600 -c 0x1 -r 'trtype:PCIe traddr:0000:00:1f.0'
