#!/bin/bash

# Script to run ReFlex setup in a container

REFLEX_HOME=`pwd`

ETH_NAME=`ifconfig | grep flags | head -n2 | tail -n1 | cut -d':' -f1`
ETH_PCI=`lspci | grep ENA | tail -n1 | awk '{print $1}'`

./conf_setup.sh
DRIVER_OVERRIDE=spdk/dpdk/igb_uio.ko $REFLEX_HOME/spdk/setup.sh

ifconfig $ETH_NAME down
$REFLEX_HOME/dpdk-devbind.py --bind=igb_uio $ETH_PCI

# mkdir -p /mnt/huge
# mount -t hugetlbfs nodev /mnt/huge
sh -c 'for i in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages; do echo 4096 > $i; done'

./dp
