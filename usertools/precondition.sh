#!/bin/bash

sudo ./deps/spdk/example/nvme/perf/perf -q 1 -s 131072 -w write -t 3600 -c 0x1 -r 'trtype:PCIe traddr:0001:01:00.0'
