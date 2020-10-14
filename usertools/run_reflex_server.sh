#!/bin/bash

# Script to run ReFlex setup after machine reboot and start the ReFlex server

modprobe uio
insmod deps/dpdk/build/kmod/igb_uio.ko
deps/dpdk/usertools/dpdk-devbind.py --bind=igb_uio 00:04.0
deps/spdk/scripts/setup.sh
sh -c 'for i in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages; do echo 4096 > $i; done'

# sudo ./dp/ix -- ./apps/reflex_server
