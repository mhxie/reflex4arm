#!/bin/bash
#
# description: Script to run ReFlex setup after machine reboot and start the ReFlex server

modprobe uio
export REFLEX_HOME=/home/ec2-user/reflex4arm
DRIVER_OVERRIDE=$REFLEX_HOME/deps/dpdk/x86_64-native-linux-gcc/kmod/igb_uio.ko $REFLEX_HOME/deps/spdk/scripts/setup.sh
# deps/spdk/scripts/setup.sh
ifconfig eth1 down
$REFLEX_HOME/deps/dpdk/usertools/dpdk-devbind.py --bind=igb_uio 0000:00:04.0
sh -c 'echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'
$REFLEX_HOME/ix
