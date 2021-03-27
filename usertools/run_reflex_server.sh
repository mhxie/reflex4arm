#!/bin/bash

# Script to run ReFlex setup after machine reboot and start the ReFlex server

export REFLEX_HOME=/home/ubuntu/reflex4arm

ifconfig ens6 down
$REFLEX_HOME/deps/dpdk/usertools/dpdk-devbind.py --bind=igb_uio 0000:00:04.0

DRIVER_OVERRIDE=deps/dpdk/build/kernel/linux/igb_uio/igb_uio.ko $REFLEX_HOME/deps/spdk/scripts/setup.sh

# mkdir -p /mnt/huge
# mount -t hugetlbfs nodev /mnt/huge
sh -c 'for i in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages; do echo 4096 > $i; done'

./build/dp
# ./dp/ix -- ./apps/reflex_server
