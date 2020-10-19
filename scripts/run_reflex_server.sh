#!/bin/bash

# Script to run ReFlex setup after machine reboot and start the ReFlex server

modprobe uio
DRIVER_OVERRIDE=deps/dpdk/build/kmod/igb_uio.ko deps/spdk/scripts/setup.sh
deps/dpdk/usertools/dpdk-devbind.py --bind=igb_uio 0:00:04.0
sh -c 'echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'
./ix
