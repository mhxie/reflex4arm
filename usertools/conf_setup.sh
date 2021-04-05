#!/bin/bash

cp ix.conf.sample ix.conf
cp sample-aws-ec2.devmodel nvme_devname.devmodel

ETH_NAME=`ifconfig | grep flags | head -n2 | tail -n1 | cut -d':' -f1`
SECOND_IP=`ifconfig $ETH_NAME | grep 'inet ' | awk '{print $2}'`
GATEWAY_IP=`ifconfig $ETH_NAME | grep 'inet ' | awk '{print $6}' | sed 's/255/1/g'`
NVME_PCI=`lspci | grep Non-Volatile | awk '{print $1}'`
ETH_PCI=`lspci | grep ENA | tail -n1 | awk '{print $1}'`
sed -i "s/10.10.66.3/$SECOND_IP/g" ix.conf # update ip
sed -i "s/10.10.66.1/$GATEWAY_IP/g" ix.conf # update ip
sed -i "s/0008:01:00.0/0000:$ETH_PCI/g" ix.conf # update ena pci
sed -i "s/0000:01:00.0/0000:$NVME_PCI/g" ix.conf # update nvme pci

# update loader
uname_p=$(uname -p)
sed -i "s/aarch64/$uname_p/g" ix.conf