#!/bin/bash

# Script to run ReFlex setup after machine reboot and start the ReFlex server

modprobe uio
DRIVER_OVERRIDE=deps/dpdk/x86_64-native-linux-gcc/kmod/igb_uio.ko deps/spdk/scripts/setup.sh
# deps/spdk/scripts/setup.sh
ifconfig eth1 down
deps/dpdk/usertools/dpdk-devbind.py --bind=igb_uio 0000:00:04.0
sh -c 'echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'
./ix &
