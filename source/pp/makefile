#keymon

ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif

TARGET = pp

CC = $(CROSS_COMPILE)gcc
CFLAGS	= -Os -lrt -ldl -Wl,--gc-sections -s
CFLAGS  += -I.

all:
	$(CC) $(TARGET).c -o $(TARGET) $(CFLAGS)
clean:
	rm -rf $(TARGET)