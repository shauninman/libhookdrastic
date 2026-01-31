BUILD_HASH:=$(shell git rev-parse --short HEAD)
RELEASE_TIME:=$(shell TZ=GMT date +%Y%m%d)
RELEASE_BETA=
RELEASE_BASE=dedicated-zero40-$(RELEASE_TIME)$(RELEASE_BETA)
RELEASE_DOT:=$(shell find ./releases/. -regex ".*/${RELEASE_BASE}-[0-9]+\.zip" | wc -l | sed 's/ //g')
RELEASE_NAME=$(RELEASE_BASE)-$(RELEASE_DOT)

all: setup build package done
	
setup:
	# make sure we're running in an input device
	tty -s 
	
	# ready fresh build
	rm -rf ./build
	mkdir -p ./releases
	cp -R ./skeleton ./build
	
	# remove authoring detritus
	cd ./build && find . -type f -name '.keep' -delete

build:
	# build
	cd source/pp && make
	cd source/hangmon && make
	cd source/libhookdrastic && make
	
package:
	# package
	cp source/pp/pp.elf ./build/system/bin/pp
	cp source/hangmon/hangmon.elf ./build/system/bin/hangmon
	cp source/libhookdrastic/libhookdrastic.so ./build/system/lib/
	cd ./build/system && echo "$(RELEASE_NAME)\n$(BUILD_HASH)" > version.txt
	cd ./build && find . -type f -name '.DS_Store' -delete
	cd ./build && zip -r ../releases/$(RELEASE_NAME).zip bios games system userdata bootlogo.bmp main.sh

done:
	# all done!
	@echo $(RELEASE_NAME)
	say "done" 2>/dev/null || true
