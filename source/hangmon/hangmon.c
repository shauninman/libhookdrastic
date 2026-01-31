#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>

#define CODE_MENU		158
#define CODE_POWER		116

#define REPEAT 2

#define INPUT_COUNT 2
static int inputs[INPUT_COUNT] = {};
static struct input_event ev;

static volatile int quit = 0;
static void on_term(int sig) { quit = 1; }

int main (int argc, char *argv[]) {
	struct sigaction sa = {0};
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);

	char path[32];
	for (int i=0; i<INPUT_COUNT; i++) {
		sprintf(path, "/dev/input/event%i", (i*2)+1); // 1 & 3
		inputs[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	}
	
	int input;
	int menu = 0;
	
	while (!quit) {
		for (int i=0; i<INPUT_COUNT; i++) {
			input = inputs[i];
			while(read(input, &ev, sizeof(ev))==sizeof(ev)) {
				if (ev.type!=EV_KEY || ev.value>REPEAT) continue;
				if (ev.code==CODE_MENU) menu = ev.value;
				if (menu && ev.code==CODE_POWER && ev.value) system("killall -s kill drastic");
			}
		}
		usleep(16666); // 60fps
	}
	
	for (int i=0; i<INPUT_COUNT; i++) {
		close(inputs[i]);
	}
}
