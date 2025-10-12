TARGET=libhookdrastic
all:
	$(CROSS_COMPILE)gcc $(TARGET).c -o $(TARGET).so -shared -lSDL2_ttf -lSDL2_image -fPIC