#!/bin/sh

export PATH=/mnt/SDCARD/system/bin:$PATH
export LD_LIBRARY_PATH=/mnt/SDCARD/system/lib:$LD_LIBRARY_PATH

# disable led
pp 0x0300B034 0xC4

# update bootlogo if changed
bootlogo

# hold MENU and press POWER to kill drastic if it hangs
hangmon &

# drastic
EXEC_LOOP=/tmp/exec_loop
touch $EXEC_LOOP
while [ -f $EXEC_LOOP ]; do
	/mnt/SDCARD/system/launch.sh
	sync
done

# persist last log
cp /tmp/drastic.txt /mnt/UDISK/
sync

# cleanup and quit
killall -s term hangmon
shutdown
