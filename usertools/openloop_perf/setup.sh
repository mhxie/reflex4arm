#!/bin/bash

ROOT_DIR=/home/ec2-user/reflex4arm
SRC_DIR=$ROOT_DIR/usertools/openloop_perf

if [[ "$1" == "apply" ]]; then
    echo "Applying this patch"
    cp $SRC_DIR/perf.c $ROOT_DIR/spdk/examples/nvme/perf
    cp $SRC_DIR/Makefile $ROOT_DIR/spdk/examples/nvme/perf
    cd $ROOT_DIR/spdk/examples/nvme/perf && sudo make && cd $ROOT_DIR/usertools
elif [[ "$1" == "recover" ]]; then
    echo "Removing this patch"
    cd $ROOT_DIR/spdk/examples/nvme/perf
    git checkout perf.c
    git checkout Makefile
    sudo make && cd $ROOT_DIR/usertools
else
    echo "Please use apply or recover"
fi
