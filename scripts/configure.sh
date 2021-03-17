#!/bin/bash

cp ix.conf.sample ix.conf
cp sample-aws-ec2.devmodel nvme_devname.devmodel

uname_p=$(uname -p)
sed -i "s/aarch64/$uname_p/g" ix.conf # update loader