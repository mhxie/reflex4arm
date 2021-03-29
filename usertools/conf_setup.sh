#!/bin/bash

cp ix.conf.sample ix.conf
cp sample-aws-ec2.devmodel nvme_devname.devmodel

SECOND_IP=`ifconfig ens6 | grep 'inet ' | awk '{print $2}'`
GATEWAY_IP=`ifconfig ens6 | grep 'inet ' | awk '{print $6}' | sed 's/255/1/g'`
sed -i "s/10.10.66.3/$SECOND_IP/g" ix.conf # update ip
sed -i "s/10.10.66.1/$GATEWAY_IP/g" ix.conf # update ip

# update loader
uname_p=$(uname -p)
sed -i "s/aarch64/$uname_p/g" ix.conf