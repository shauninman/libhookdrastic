#!/bin/sh

SDCARD_PATH=/mnt/SDCARD
DIR=$SDCARD_PATH/system/drastic
LIB=$SDCARD_PATH/system/lib

cd $DIR
export HOME=$DIR
mkdir -p $SDCARD_PATH/userdata
LD_PRELOAD=$LIB/libhookdrastic.so ./drastic > /tmp/drastic.txt 2>&1
