#!/bin/sh

export PATH=/mnt/SDCARD/system/bin:$PATH
export LD_LIBRARY_PATH=/mnt/SDCARD/system/lib:$LD_LIBRARY_PATH

# update
if [ -f /mnt/SDCARD/system.zip ]; then
	cd /mnt/SDCARD/
	rm -rf system
	unzip -q system.zip
fi

# loop
/mnt/SDCARD/system/loop.sh