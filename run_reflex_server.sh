#!/bin/bash

# Script to run ReFlex setup after machine reboot and start the ReFlex server


sudo insmod deps/dpdk/build/kmod/igb_uio.ko
sudo deps/dpdk/usertools/dpdk-devbind.py --bind=igb_uio 0008:01:00.0
sudo sh -c 'echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'
sudo deps/spdk/scripts/setup.sh
# sudo ./dp/ix -- ./apps/reflex_server
