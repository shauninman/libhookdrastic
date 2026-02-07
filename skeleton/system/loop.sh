#!/bin/sh

# disable led
led off

# update bootlogo if changed
bootlogo

# hold MENU and press POWER to kill drastic if it hangs
hangmon &

# drastic
EXEC_LOOP=/tmp/exec_loop
touch $EXEC_LOOP
while [ -f $EXEC_LOOP ]; do
	/mnt/SDCARD/system/run.sh
	sync
done

# persist last log
cp /tmp/drastic.txt /mnt/UDISK/
sync

# cleanup and quit
killall -s term hangmon

led on
shutdown
