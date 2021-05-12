#!/bin/bash

# Script to run ReFlex setup after machine reboot and start the ReFlex server

REFLEX_HOME=`pwd`
ETH_NAME=`ifconfig | grep flags | head -n2 | tail -n1 | cut -d':' -f1`
ETH_PCI=`lspci | grep ENA | tail -n1 | awk '{print $1}'`

DRIVER_OVERRIDE=spdk/dpdk/build-tmp/kernel/linux/igb_uio/igb_uio.ko $REFLEX_HOME/spdk/scripts/setup.sh

ifconfig $ETH_NAME down
$REFLEX_HOME/spdk/dpdk/usertools/dpdk-devbind.py --bind=igb_uio $ETH_PCI

# mkdir -p /mnt/huge
# mount -t hugetlbfs nodev /mnt/huge
sh -c 'for i in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages; do echo 4096 > $i; done'

# $REFLEX_HOME/build/dp
# ./dp/ix -- ./apps/reflex_server
