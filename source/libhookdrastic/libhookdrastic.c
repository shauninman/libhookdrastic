#define _GNU_SOURCE // for RTLD_NEXT
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <setjmp.h>
#include <dirent.h>
#include <sys/ioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <alsa/asoundlib.h>

// --------------------------------------------

#define CEIL_TO(x, n) (((x) + (n) - 1) / (n) * (n))
#define NUMBER_OF(items) (sizeof(items) / sizeof((items)[0]))

#define SDCARD_PATH		"/mnt/SDCARD"
#define GAMES_PATH 		SDCARD_PATH "/games"
#define ARCHIVE_PATH	GAMES_PATH "/archive"
#define SYSTEM_PATH 	SDCARD_PATH "/system"
#define ASSETS_PATH		SYSTEM_PATH "/assets"
#define USERDATA_PATH	SDCARD_PATH "/userdata"

#define BAT_PATH 		"/sys/class/power_supply/axp2202-battery/"
#define USB_PATH 		"/sys/class/power_supply/axp2202-usb/"
#define CPU_PATH		"/sys/devices/system/cpu/cpu0/cpufreq/"

#define FREQ_MENU		"408000"
#define FREQ_GAME		"1800000"

#define MAX_LINE 1024
#define MAX_PATH 512
#define MAX_FILE 256

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 800
#define SCREEN_COUNT 2 // duh

#define BLACK_TRIAD 	0x00,0x00,0x00
#define WHITE_TRIAD 	0xff,0xff,0xff
#define LIGHT_TRIAD 	0xbb,0xbb,0xbb
#define DARK_TRIAD 		0x33,0x33,0x33
#define RED_TRIAD		0xff,0x33,0x33
#define GREEN_TRIAD		0x33,0xcc,0x33
#define YELLOW_TRIAD	0xff,0xcc,0x00

#define TRIAD_ALPHA(t,a) (SDL_Color){t,a}

#define BLACK_COLOR		TRIAD_ALPHA(BLACK_TRIAD,0xff)
#define WHITE_COLOR		TRIAD_ALPHA(WHITE_TRIAD,0xff)
#define LIGHT_COLOR		TRIAD_ALPHA(LIGHT_TRIAD,0xff)
#define DARK_COLOR		TRIAD_ALPHA(DARK_TRIAD,0xff)
#define RED_COLOR		TRIAD_ALPHA(RED_TRIAD,0xff)
#define GREEN_COLOR		TRIAD_ALPHA(GREEN_TRIAD,0xff)
#define YELLOW_COLOR	TRIAD_ALPHA(YELLOW_TRIAD,0xff)

// --------------------------------------------

#define JOY_UP 		12
#define JOY_DOWN	15
#define JOY_LEFT	13
#define JOY_RIGHT	14
#define JOY_X		2
#define JOY_B		1
#define JOY_Y		3
#define JOY_A		11
#define JOY_START	9
#define JOY_SELECT	8
#define JOY_MENU	18
#define JOY_L1		4
#define JOY_R1		5
#define JOY_L2		6
#define JOY_R2		7
#define JOY_L3		10

#define JOY_PLUS	17
#define JOY_MINUS	16

#define AXIS_Y 	0
#define AXIS_X 	1

#define SCAN_POWER	102

// --------------------------------------------

typedef enum {
	OSD_NONE,
	OSD_VOLUME,
	OSD_BRIGHTNESS,
} OSDMode;

typedef enum {
	MODE_MENU,
	MODE_ARCHIVE,
} MenuMode;

typedef struct {
	char name[MAX_FILE];
	int hidden;
} Entry;

static struct {
	uintptr_t base;
	
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* screens[SCREEN_COUNT];
	SDL_Texture* preview[SCREEN_COUNT];
	SDL_Texture* overlay;
	SDL_Texture* eye;
	SDL_Texture* bg;
	SDL_Rect rects[SCREEN_COUNT];
	
	int count;
	int current;
	int capacity;
	Entry* items;
	
	char game_path[MAX_PATH];
	char game_name[MAX_FILE];
	
	int bat;
	int usb;
	int batmon;
	
	int synced;
	int menu;
	int osd;
	int capture;
	int fast_forward;
} app;

static int osd_at;

// --------------------------------------------
// raw HMI controls
// --------------------------------------------

static void raw_vol(int value) {
	char* card_name = "hw:0";
	char* elem_name = "DAC volume";
	char* mute_name = "HpSpeaker";

	snd_mixer_t *mixer = NULL;
	snd_mixer_open(&mixer, 0);
	snd_mixer_attach(mixer, card_name);
	snd_mixer_selem_register(mixer, NULL, NULL);
	snd_mixer_load(mixer);

	snd_mixer_selem_id_t *sid = NULL;
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, elem_name);
	snd_mixer_elem_t *elem = snd_mixer_find_selem(mixer, sid);
	snd_mixer_selem_set_playback_volume_all(elem, value);

	snd_mixer_close(mixer);
}

#define DISP_LCD_SET_BRIGHTNESS 0x102
static void raw_bri(int value) {
	int fd = open("/dev/disp", O_RDWR);
	if (fd<0) return;

	unsigned long param[4]={0,value,0,0};
	ioctl(fd, DISP_LCD_SET_BRIGHTNESS, &param);
	close(fd);
}

#define LED_ADDR ((uintptr_t)0x0300B034)
#define LED_ON	0xC0
#define LED_OFF 0xC4
static void raw_led(int enable) {
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd<0) return;
	
	size_t page_size = getpagesize();
	off_t base = (off_t)(LED_ADDR & ~(page_size - 1));

	uint8_t *map = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
	if (map!=MAP_FAILED) {
		volatile uint32_t *reg = (uint32_t *)(map + (LED_ADDR - base));
		*reg = enable ? LED_ON : LED_OFF;
		munmap(map, page_size);
	}
	close(fd);
}

// --------------------------------------------
// settings
// --------------------------------------------

static struct {
	int version;
	int volume;		// 0-20
	int brightness;	// 0 - 10
	int cropped;
	int spread;
	char game[MAX_PATH];
} settings = {
	.version = 1,
	.volume = 6,
	.brightness = 3,
	.cropped = 1,
	.spread = 1,
};

#define SETTINGS_PATH USERDATA_PATH "/settings.bin"
static void Settings_load(void) {
	raw_vol(0);
	
	if (access(SETTINGS_PATH, F_OK)!=0) return;
	
	FILE* file = fopen(SETTINGS_PATH, "rb");
	if (!file) return;
	fread(&settings, sizeof(settings), 1, file);
	fclose(file);
}
static void Settings_save(void) {
	FILE* file = fopen(SETTINGS_PATH, "wb");
	if (!file) return;
	fwrite(&settings, sizeof(settings), 1, file);
	fclose(file);
}
static void Settings_setVolume(int value) {
	char cmd[MAX_FILE];
	static const uint8_t raw[21] = {
		  0, // mute
		120, // 0
		131, // 11
		138, // 7
		143, // 5
		147, // 4
		151, // 4
		154, // 3
		157, // 3
		160, // 3
		162, // 2
		164, // 2
		166, // 2
		168, // 2
		170, // 2
		172, // 2
		174, // 2
		176, // 2
		178, // 2
		179, // 2
		180, // 1
	};
	
	raw_vol(raw[value]);
	settings.volume = value;
}
static void Settings_setBrightness(int value) {
	char cmd[MAX_FILE];
	static const uint8_t raw[11] = {
		  1, // 0
		  8, // 8
		 16, // 8
		 32, // 16
		 48, // 16
		 72, // 24
		 96, // 24
		128, // 32
		160, // 32
		192, // 32
		255, // 64
	};
	
	raw_bri(raw[value]);
	settings.brightness = value;
}

// --------------------------------------------
// real function handles
// --------------------------------------------

static int (*real_SDL_Init)(Uint32) = NULL;
static SDL_Window* (*real_SDL_CreateWindow)(const char*, int, int, int, int, Uint32) = NULL;
static void (*real_SDL_SetWindowSize)(SDL_Window *, int, int) = NULL;
static int (*real_SDL_RenderSetLogicalSize)(SDL_Renderer*, int, int) = NULL;
static SDL_Renderer* (*real_SDL_CreateRenderer)(SDL_Window*, int, Uint32) = NULL;
static int (*real_SDL_RenderClear)(SDL_Renderer *) = NULL;
static void (*real_SDL_RenderPresent)(SDL_Renderer*) = NULL;
static SDL_Texture* (*real_SDL_CreateTexture)(SDL_Renderer*, Uint32, int, int, int) = NULL;
static void (*real_SDL_DestroyTexture)(SDL_Texture *) = NULL;
static int (*real_SDL_RenderCopy)(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*) = NULL;
static int (*real_SDL_OpenAudio)(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) = NULL;
static int (*real_SDL_PollEvent)(SDL_Event*) = NULL;
static void (*real_SDL_Delay)(uint32_t) = NULL;

static int (*real__libc_start_main)(int (*main)(int,char**,char**), int argc, char **ubp_av, void (*init)(void), void (*fini)(void), void (*rtld_fini)(void), void *stack_end) = NULL;
static int (*real__sprintf_chk)(char *s, int flag, size_t slen, const char *fmt, ...) = NULL;
static int (*real__snprintf_chk)(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...) = NULL;
static void (*real_exit)(int) __attribute__((noreturn)) = NULL;
static void (*real__exit)(int) __attribute__((noreturn)) = NULL;
static int (*real_system)(const char *) = NULL;
static int (*real_puts)(const char *__s) = NULL;
static int (*real__printf_chk)(int flag, const char *fmt, ...) = NULL;

// --------------------------------------------
// TODO: tmp? SDL2 compat
// --------------------------------------------

typedef enum SDL_ScaleMode
{
	SDL_ScaleModeNearest, /**< nearest pixel sampling */
	SDL_ScaleModeLinear,  /**< linear filtering */
	SDL_ScaleModeBest	 /**< anisotropic filtering */
} SDL_ScaleMode;
int SDL_SetTextureScaleMode(SDL_Texture * texture, SDL_ScaleMode scaleMode);

// --------------------------------------------
// logging
// --------------------------------------------

#define LOG(...) printf(__VA_ARGS__); printf("\n"); fflush(stdout)
static inline void LOG_event(SDL_Event* event) {
	switch (event->type) {
		case SDL_QUIT:
			printf("SDL_PollEvent: QUIT\n"); 
			break;
		
		case SDL_KEYDOWN:
			printf("SDL_PollEvent: KEYDOWN (scancode=%d sym=%d)\n", event->key.keysym.scancode, event->key.keysym.sym);
			break;
		case SDL_KEYUP:
			printf("SDL_PollEvent: KEYUP (scancode=%d sym=%d)\n", event->key.keysym.scancode, event->key.keysym.sym);
			break;
		
		case SDL_MOUSEBUTTONDOWN:
			printf("SDL_PollEvent: MOUSEBUTTONDOWN (button=%d)\n", event->button.button);
			break;
		case SDL_MOUSEBUTTONUP:
			printf("SDL_PollEvent: MOUSEBUTTONUP (button=%d)\n", event->button.button);
			break;
		case SDL_MOUSEMOTION:
			printf("SDL_PollEvent: MOUSEMOTION (x=%d y=%d)\n", event->motion.x, event->motion.y);
			break;
			
		case SDL_FINGERDOWN:
			printf("SDL_PollEvent: FINGERDOWN (fingerId=%lld touchId=%lld x=%f y=%f pressure=%f)\n", (long long)event->tfinger.fingerId, (long long)event->tfinger.touchId, event->tfinger.x, event->tfinger.y, event->tfinger.pressure);
			break;
		case SDL_FINGERUP:
			printf("SDL_PollEvent: FINGERUP (fingerId=%lld touchId=%lld x=%f y=%f pressure=%f)\n", (long long)event->tfinger.fingerId, (long long)event->tfinger.touchId, event->tfinger.x, event->tfinger.y, event->tfinger.pressure);
			break;
		case SDL_FINGERMOTION:
			printf("SDL_PollEvent: FINGERMOTION (fingerId=%lld touchId=%lld x=%f y=%f dx=%f dy=%f pressure=%f)\n", (long long)event->tfinger.fingerId, (long long)event->tfinger.touchId, event->tfinger.x, event->tfinger.y, event->tfinger.dx, event->tfinger.dy, event->tfinger.pressure);
			break;

		case SDL_JOYAXISMOTION:
			// printf("SDL_PollEvent: JOYAXISMOTION which=%d axis=%d value=%d\n", event->jaxis.which, event->jaxis.axis, event->jaxis.value);
			break;
		case SDL_JOYBUTTONDOWN:
			printf("SDL_PollEvent: JOYBUTTONDOWN which=%d button=%d\n", event->jbutton.which, event->jbutton.button);
			break;
		case SDL_JOYBUTTONUP:
			printf("SDL_PollEvent: JOYBUTTONUP which=%d button=%d\n", event->jbutton.which, event->jbutton.button);
			break;
		case SDL_JOYHATMOTION:
			printf("SDL_PollEvent: JOYHATMOTION which=%d hat=%d value=%d\n", event->jhat.which, event->jhat.hat, event->jhat.value);
			break;

		case SDL_JOYDEVICEADDED:
			printf("SDL_PollEvent: JOYDEVICEADDED which=%d\n", event->jdevice.which);
			break;
		case SDL_JOYDEVICEREMOVED:
			printf("SDL_PollEvent: JOYDEVICEREMOVED which=%d\n", event->jdevice.which);
			break;

		case SDL_CONTROLLERAXISMOTION:
			printf("SDL_PollEvent: CONTROLLERAXISMOTION which=%d axis=%d value=%d\n", event->caxis.which, event->caxis.axis, event->caxis.value);
			break;
		case SDL_CONTROLLERBUTTONDOWN:
			printf("SDL_PollEvent: CONTROLLERBUTTONDOWN which=%d button=%d\n", event->cbutton.which, event->cbutton.button);
			break;
		case SDL_CONTROLLERBUTTONUP:
			printf("SDL_PollEvent: CONTROLLERBUTTONUP which=%d button=%d\n", event->cbutton.which, event->cbutton.button);
			break;

		case SDL_CONTROLLERDEVICEADDED:
			printf("SDL_PollEvent: CONTROLLERDEVICEADDED which=%d\n", event->cdevice.which);
			break;
		case SDL_CONTROLLERDEVICEREMOVED:
			printf("SDL_PollEvent: CONTROLLERDEVICEREMOVED which=%d\n", event->cdevice.which);
			break;
		
		default:
			printf("SDL_PollEvent: type=%d\n", event->type);
			break;
	}
	fflush(stdout);
}
static inline void LOG_rect(const char *label, const SDL_Rect *r) {
	if (r) printf("%s[x=%d y=%d w=%d h=%d] ", label, r->x, r->y, r->w, r->h);
	else   printf("%sNULL ", label);
}
static inline void LOG_render(SDL_Renderer *renderer, SDL_Texture  *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect) {
	float sx=1.0f, sy=1.0f;
	int lw=0, lh=0, outw=0, outh=0;
	SDL_Rect vp; vp.x=vp.y=vp.w=vp.h=0;
	SDL_bool integer = SDL_FALSE;

	SDL_RenderGetScale(renderer, &sx, &sy);				  // 1,1 unless set
	SDL_RenderGetLogicalSize(renderer, &lw, &lh);			// 0,0 if none
	integer = SDL_RenderGetIntegerScale(renderer);		   // SDL_TRUE if forced integer scale
	SDL_RenderGetViewport(renderer, &vp);					// defaults to full output
	SDL_GetRendererOutputSize(renderer, &outw, &outh);	   // e.g. 1024x768

	printf("SDL_RenderCopy: renderer=%p texture=%p ", (void*)renderer, (void*)texture);
	LOG_rect("src=", srcrect);
	LOG_rect("dst=", dstrect);
	printf(" | scale=%.3fx%.3f logical=%dx%d integer=%d viewport=[%d,%d %dx%d] output=%dx%d\n", sx, sy, lw, lh, integer ? 1 : 0, vp.x, vp.y, vp.w, vp.h, outw, outh);
	fflush(stdout);
}
static inline void LOG_texture(SDL_Texture* texture, const SDL_Rect* rect, int pitch) {
	Uint32 fmt = 0; int access = 0, tw = 0, th = 0;
	SDL_QueryTexture(texture, &fmt, &access, &tw, &th);

	int rw = rect ? rect->w : tw;
	int rh = rect ? rect->h : th;
	int bpp = SDL_BYTESPERPIXEL(fmt);
	long long approx = (long long)rh * pitch;
	
	printf("Texture %p ", (void*)texture);
	LOG_rect("rect=", rect);
	printf("pitch=%d format=%s tex=%dx%d bytes=%lld\n", pitch, SDL_GetPixelFormatName(fmt), tw, th, approx);
	fflush(stdout);
}
static const char* get_access_name(int access) {
	switch (access) {
		case SDL_TEXTUREACCESS_STATIC:   return "STATIC";
		case SDL_TEXTUREACCESS_STREAMING:return "STREAMING";
		case SDL_TEXTUREACCESS_TARGET:   return "TARGET";
		default:						 return "UNKNOWN";
	}
}
static void hexdump(const void *ptr, size_t len) {
	const unsigned char *p = (const unsigned char *)ptr;
	for (size_t i = 0; i<len; i += 16) {
		printf("%08zx  ", i);
		for (size_t j = 0; j<16; ++j) {
			if (i + j<len) printf("%02X ", p[i + j]);
			else			 printf("   ");
		}
		printf(" ");
		for (size_t j = 0; j<16 && i + j<len; ++j) {
			unsigned char c = p[i + j];
			printf("%c", (c>=32 && c<127) ? c : '.');
		}
		printf("\n");
	}
	fflush(stdout);
}

// --------------------------------------------
// loading
// --------------------------------------------

enum {
	LOADER_IDLE = 0,
	LOADER_REQUESTED,
	LOADER_AWAITING,
	LOADER_STARTED,
	LOADER_COMPLETE,
};
enum {
	LOADER_RESET = 0,
	LOADER_RESUME,
};

static struct {
	int state;
	int after;
} loader = {LOADER_AWAITING,LOADER_RESUME};

// --------------------------------------------
// drastic shims
// --------------------------------------------

#define PTR_AT(ptr) (void*)(*(uintptr_t *)(ptr))
#define GET_PFN(ptr) (void*)((ptr))

typedef int (*drastic_main_t)(int,char**,char**);
typedef void (*drastic_quit_t)(void*);
typedef void (*drastic_reset_system_t)(void*);
typedef int32_t (*drastic_load_state_t)(void *, const char *, uint16_t *, uint16_t *, uint32_t);
typedef int32_t (*drastic_save_state_t)(void *, const char *, char *, uint16_t *, uint16_t *);
typedef int32_t (*drastic_load_nds_t)(void *, const char *);
typedef uint8_t (*drastic_audio_pause_t)(void *);
typedef void (*drastic_audio_unpause_t)(void *);
typedef void (*drastic_audio_revert_t)(void *);

static inline void* drastic_var_system(void) {
	return PTR_AT(app.base + 0x15ff30); // follow arg in Cutter
}
static void drastic_audio_pause(int flag) {
	// not sure this is necessary but
	// it feels like good hygiene?
	void* sys = drastic_var_system();
	void* audio = (uint8_t *)sys + 0x1587000;
	
	if (flag) {
		drastic_audio_pause_t drastic_audio_pause = GET_PFN(app.base + 0x0008caa0);
		drastic_audio_pause(audio);
	}
	else {
		drastic_audio_unpause_t drastic_audio_unpause = GET_PFN(app.base + 0x0008caf0);
		drastic_audio_unpause(audio);
		// TODO: this seems to cause hangs
		// drastic_audio_revert_t drastic_audio_revert = GET_PFN(app.base + 0x000886a0);
		// drastic_audio_revert(audio);
	}
}
static inline int drastic_is_saving(void) {
	const volatile uint32_t *busy = (const volatile uint32_t *)(app.base + 0x3ec27c);
	return __atomic_load_n(busy, __ATOMIC_ACQUIRE)!=0;
}
static void drastic_await_save(void) {
	while (drastic_is_saving()) real_SDL_Delay(1);
	sync();
}
static void drastic_load_nds_and_jump(const char* path) {
	drastic_await_save();

	drastic_load_nds_t d_load_nds = GET_PFN(app.base + 0x0006fd30); // nm ./drastic | grep load_nds
	drastic_reset_system_t d_reset_system = GET_PFN(app.base + 0x0000fd50); // nm ./drastic | grep reset_system

	void* sys = drastic_var_system();
	d_load_nds((uint8_t *)sys + 800, path);
	d_reset_system(sys);
	
	jmp_buf *env = (jmp_buf *)((uint8_t *)sys + 0x3b2a840); // from main in Cutter
	longjmp(*env, 1); // no return
}
static void drastic_load_state(int slot) {
	drastic_await_save();

	void* sys = drastic_var_system();	
	drastic_load_state_t d_load_state = GET_PFN(app.base + 0x000746f0); // nm ./drastic | grep load_state
	
	char path[MAX_PATH];
	sprintf(path, USERDATA_PATH "/states/%s.st%i", app.game_name, slot);

	d_load_state(sys, path, NULL,NULL,0);
}
static void drastic_save_state(int slot) {
	drastic_await_save();

	void* sys = drastic_var_system();	
	drastic_save_state_t d_save_state = GET_PFN(app.base + 0x00074da0); // nm ./drastic | grep save_state
	
	char name[MAX_FILE];
	sprintf(name, "%s.st%i", app.game_name, slot);
	
	d_save_state(sys, USERDATA_PATH "/states/", name, NULL,NULL);
}
static void drastic_quit(void) {
	drastic_await_save();
	drastic_quit_t d_quit = GET_PFN(app.base + 0x0000e8d0); // nm ./drastic | grep quit
	void* sys = drastic_var_system();	
	d_quit(sys);
}

// --------------------------------------------
// support
// --------------------------------------------

static inline int getInt(int f) {
	if (f<0) return 0;

	char b[32];
	int n = pread(f, b, sizeof(b) - 1, 0);
	if (n<=0) return 0;

	b[n] = '\0';
	return atoi(b);
}
static inline void getString(void) {
	
}
static inline void putString(const char* path, const char* value) {
	int f = open(path, O_WRONLY);
	if (f<0) return;
	write(f, value, strlen(value));
	close(f);
}
static inline void putInt(const char* path, int value) {
	char buffer[16];
	sprintf(buffer, "%d", value);
	putString(path, buffer);
}
static inline int exists(const char* path) {
	return access(path, F_OK)==0;
}

static int compareNatural(const char *a, const char *b) {
	while (*a && *b) {
		// ensure "Game 10.ext" sorts after "Game 2.ext"
		if (isdigit(*a) && isdigit(*b)) {
			char *ea, *eb;
			long na = strtol(a, &ea, 10);
			long nb = strtol(b, &eb, 10);
			
			if (na!=nb) return (na<nb) ? -1 : 1;

			a = ea;
			b = eb;
		} else {
			int ca = tolower(*a);
			int cb = tolower(*b);
			
			if (ca!=cb) {
				// special case: treat '.' as less than ' '
				// ensure "Game.ext" sorts before "Game 2.ext"
				if (ca=='.' && cb==' ') return -1;
				if (ca==' ' && cb=='.') return  1;
				return ca - cb;
			}
			
			a++;
			b++;
		}
	}
	return tolower(*a) - tolower(*b);
}

// --------------------------------------------
// button repeater
// --------------------------------------------

#define REPEAT_TIMEOUT	300
#define REPEAT_INTERVAL 100

static int Repeater_fakeButtonEvent(SDL_Event* event, int btn, int press) {
	SDL_memset(event, 0, sizeof(*event));
	event->type = event->jbutton.type = press ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
	event->jbutton.which  = 0;
	event->jbutton.button = btn;
	event->jbutton.state  = press ? SDL_PRESSED : SDL_RELEASED;
	event->jbutton.timestamp = SDL_GetTicks();
	return 1;
}
static int Repeater_pollEvent(SDL_Event* event) {
	static int menu_down = 0;
	static int plus_next = 0;
	static int minus_next = 0;
	static int r1_next = 0;
	static int l1_next = 0;
	
	// analog to dpad
	static int ax = 0;
	static int ay = 0;
	static int dpad_up = 0;
	static int dpad_down = 0;
	static int dpad_left = 0;
	static int dpad_right = 0;
	
	int result = 0;
	
	int now = SDL_GetTicks();
	if (plus_next && now>=plus_next) {
		result = Repeater_fakeButtonEvent(event, JOY_PLUS, 1);
		plus_next += REPEAT_INTERVAL;
	}
	else if (minus_next && now>=minus_next) {
		result = Repeater_fakeButtonEvent(event, JOY_MINUS, 1);
		minus_next += REPEAT_INTERVAL;
	}
	else if (menu_down && r1_next && now>=r1_next) {
		result = Repeater_fakeButtonEvent(event, JOY_R1, 1);
		r1_next += REPEAT_INTERVAL;
	}
	else if (menu_down && l1_next && now>=l1_next) {
		result = Repeater_fakeButtonEvent(event, JOY_L1, 1);
		l1_next += REPEAT_INTERVAL;
	}
	else {
		result = real_SDL_PollEvent(event);
	}

	if (!result) return 0;
	
	if (event->type==SDL_JOYBUTTONDOWN) {
		int now = SDL_GetTicks();

		if (event->jbutton.button==JOY_MENU)					menu_down = 1;
		if (!plus_next && event->jbutton.button==JOY_PLUS)		plus_next = now + REPEAT_TIMEOUT;
		if (!minus_next && event->jbutton.button==JOY_MINUS)	minus_next = now + REPEAT_TIMEOUT;
		if (!r1_next && event->jbutton.button==JOY_R1)			r1_next = now + REPEAT_TIMEOUT;
		if (!l1_next && event->jbutton.button==JOY_L1)			l1_next = now + REPEAT_TIMEOUT;
	}
	else if (event->type==SDL_JOYBUTTONUP) {
		if (event->jbutton.button==JOY_MENU) 	menu_down = 0;
		if (event->jbutton.button==JOY_PLUS) 	plus_next = 0;
		if (event->jbutton.button==JOY_MINUS)	minus_next = 0;
		if (event->jbutton.button==JOY_R1)		r1_next = 0;
		if (event->jbutton.button==JOY_L1)		l1_next = 0;
	}
	else if (event->type==SDL_JOYAXISMOTION) {
		int lx = ax;
		int ly = ay;
		
		if (event->jaxis.axis==AXIS_X) ax = event->jaxis.value * -1; // inverted!
		else ay = event->jaxis.value;
		
		#define DEADZONE 12000
		if (ax!=lx) {
			if (ax>DEADZONE) {
				if (dpad_left) Repeater_fakeButtonEvent(event, JOY_LEFT, 0);
				if (!dpad_right) Repeater_fakeButtonEvent(event, JOY_RIGHT, 1);
				dpad_left = 0;
				dpad_right = 1;
			}
			else if (ax<-DEADZONE) {
				if (dpad_right) Repeater_fakeButtonEvent(event, JOY_RIGHT, 0);
				if (!dpad_left) Repeater_fakeButtonEvent(event, JOY_LEFT, 1);
				dpad_right = 0;
				dpad_left = 1;
			}
			else {
				if (dpad_left) Repeater_fakeButtonEvent(event, JOY_LEFT, 0);
				if (dpad_right) Repeater_fakeButtonEvent(event, JOY_RIGHT, 0);
				dpad_left = dpad_right = 0;
			}
		}
		if (ay!=ly) {
			if (ay>DEADZONE) {
				if (dpad_up) Repeater_fakeButtonEvent(event, JOY_UP, 0);
				if (!dpad_down) Repeater_fakeButtonEvent(event, JOY_DOWN, 1);
				dpad_up = 0;
				dpad_down = 1;
			}
			else if (ay<-DEADZONE) {
				if (dpad_down) Repeater_fakeButtonEvent(event, JOY_DOWN, 0);
				if (!dpad_up) Repeater_fakeButtonEvent(event, JOY_UP, 1);
				dpad_down = 0;
				dpad_up = 1;
			}
			else {
				if (dpad_up) Repeater_fakeButtonEvent(event, JOY_UP, 0);
				if (dpad_down) Repeater_fakeButtonEvent(event, JOY_DOWN, 0);
				dpad_up = dpad_down = 0;
			}
		}
	}
	
	return result;
}

// --------------------------------------------
// custom fonts
// --------------------------------------------

typedef struct {
	uint8_t map[128];
	uint8_t char_widths[128];
	int8_t kern_pairs[128][128];

	SDL_Surface* bitmap;
	const char* name;
	const char* charset;
	int8_t offset_x;
	int8_t offset_y;
	int8_t tracking;
	uint8_t tile_width;
	uint8_t tile_height;
	uint8_t tiles_wide;
	uint8_t tiles_high;
	uint8_t missing;

	uint8_t char_width;
	uint8_t low_width;
} Font;

Font* font36 = &(Font){
	.name = "font-dedicated-36.png",
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+% ",
	.tile_width = 36,
	.tile_height = 36,
	.offset_x = -2,
	.tracking = 6,
	.char_width = 30,
	.low_width = 24,
	.char_widths = {
		['I'] = 6,
		['1'] = 18,
		['.'] = 6,
		['-'] = 24,
		['\''] = 6,
		['!'] = 6,
		['&'] = 36,
		[' '] = 12,
	},
	.kern_pairs = {
		['A']['T'] = -6,
		['T']['A'] = -6,
		['A']['V'] = -6,
		['V']['A'] = -6,
		['A']['Y'] = -6,
		['Y']['A'] = -6,
		['T']['-'] = -6,
		['-']['T'] = -6,
	},
};

Font* font24 = &(Font){
	.name = "font-dedicated-24.png",
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+% ",
	.tile_width = 24,
	.tile_height = 24,
	.offset_x = -1,
	.tracking = 4,
	.char_width = 20,
	.char_widths = {
		['I'] = 4,
		['1'] = 12,
		['.'] = 4,
		['-'] = 16,
		['\''] = 4,
		['!'] = 4,
		[' '] = 8,
		['%'] = 21,
	},
	.kern_pairs = {
		['A']['T'] = -4,
		['T']['A'] = -4,
		['A']['V'] = -4,
		['V']['A'] = -4,
		['T']['-'] = -4,
		['-']['T'] = -4,
	},
};

Font* font18 = &(Font){
	.name = "font-dedicated-18.png",
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+% ",
	.tile_width = 18,
	.tile_height = 18,
	.tracking = 3,
	.char_width = 15,
	.char_widths = {
		['I'] = 3,
		['1'] = 9,
		['.'] = 3,
		['-'] = 12,
		['\''] = 3,
		['!'] = 3,
		[' '] = 6,
	},
	.kern_pairs = {
		['A']['T'] = -3,
		['T']['A'] = -3,
		['A']['V'] = -3,
		['V']['A'] = -3,
		['T']['-'] = -3,
		['-']['T'] = -3,
	},
};

Font* font16 = &(Font){
	.name = "font-dedicated-16.png",
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+% ",
	.tile_width = 16,
	.tile_height = 16,
	.tracking = 4,
	.char_width = 12,
	.char_widths = {
		['I'] = 2,
		['1'] = 8,
		['.'] = 2,
		['-'] = 10,
		['\''] = 2,
		['!'] = 2,
		[' '] = 4,
	},
	.kern_pairs = {
		['A']['T'] = -2,
		['T']['A'] = -2,
		['A']['V'] = -2,
		['V']['A'] = -2,
		['T']['-'] = -2,
		['-']['T'] = -2,
	},
};

void Font_init(Font* font) {
	char path[MAX_FILE];
	sprintf(path, "%s/%s", ASSETS_PATH, font->name);
	font->bitmap = IMG_Load(path);
	SDL_SetSurfaceBlendMode(font->bitmap, SDL_BLENDMODE_BLEND);
	
	font->tiles_wide = font->bitmap->w / font->tile_width;
	font->tiles_high = font->bitmap->h / font->tile_height;
	font->missing = (font->tiles_wide * font->tiles_high) - 1;
	
	memset(font->map, font->missing, sizeof(font->map));
	for (uint8_t i=0; font->charset[i]; i++) {
		unsigned char c = font->charset[i];
		font->map[c] = (uint8_t)i;
		if (!font->char_widths[c]) {
			if (font->low_width && c>='a' && c<='z') {
				font->char_widths[c] = font->low_width;
			}
			else {
				font->char_widths[c] = font->char_width;
			}
		}
	}
}
void Font_quit(Font* font) {
	SDL_FreeSurface(font->bitmap);
}

static void __Font_drawChar(Font* font, SDL_Surface* dst, unsigned char c, int x, int y) {
	int i = c<128 ? font->map[c] : font->missing;
	int tx = i % font->tiles_wide;
	int ty = i / font->tiles_wide;
	int tw = font->tile_width;
	int th = font->tile_height;
	SDL_BlitSurface(font->bitmap, &(SDL_Rect){tx*tw,ty*th,tw,th}, dst, &(SDL_Rect){x,y,tw,th});
}

void __Font_drawText(Font* font, const char* text, int* out_width, int* out_height, SDL_Surface** out_surface) {
	SDL_Surface* dst = NULL;
	if (out_surface) {
		int w = 0;
		int h = 0;
		__Font_drawText(font, text, &w, &h, NULL);
		dst = SDL_CreateRGBSurfaceWithFormat(0, w,h, 32, SDL_PIXELFORMAT_RGBA32);
		SDL_FillRect(dst, NULL, 0);
	}
	
	const char* tmp = text;
	int ow = 0;
	int oh = font->tile_height;
	while (*tmp) {
		unsigned char c = *tmp++;
		unsigned char n = *tmp;
		if (!font->char_widths[c]) c = toupper(c);
		if (n && !font->char_widths[n]) n = toupper(n);
		
		if (out_surface) __Font_drawChar(font, dst, c, ow,0);

		ow += font->char_widths[c] ? font->char_widths[c] : font->char_width;
		if (n) {
			ow += font->tracking;
			if (font->kern_pairs[c][n]) {
				ow += font->kern_pairs[c][n];
			}
			else {
				ow += font->kern_pairs[0][n];
			}
		}
	}
	ow -= font->offset_x; // reverse pad?
	
	if (out_width) *out_width = ow;
	if (out_height) *out_height = oh;
	if (out_surface) *out_surface = dst;
}

void Font_getTextSize(Font* font, const char* text, int* out_width, int* out_height) {
	__Font_drawText(font, text, out_width, out_height, NULL);
}

SDL_Surface* Font_drawText(Font* font, const char* text) {
	SDL_Surface* surface = NULL;
	__Font_drawText(font, text, NULL, NULL, &surface);
	return surface;
}

typedef void (*Font_renderFunc)(SDL_Renderer* renderer, Font* font, const char* text, int x, int y, SDL_Color color);

void __Font_renderText(SDL_Renderer* renderer, Font* font, const char* text, int x, int y, SDL_Color color, int s) {	
	x += font->offset_x;
	y += font->offset_y;

	SDL_Surface* surface = Font_drawText(font, text);
	SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
	
	if (s) {
		SDL_SetTextureColorMod(texture, 0,0,0);
		real_SDL_RenderCopy(renderer, texture, NULL, &(SDL_Rect){x+2,y+2,surface->w,surface->h});
	}
	
	SDL_SetTextureColorMod(texture, color.r, color.g, color.b);
	real_SDL_RenderCopy(renderer, texture, NULL, &(SDL_Rect){x,y,surface->w,surface->h});

	SDL_DestroyTexture(texture);
	SDL_FreeSurface(surface);
}

void Font_renderText(SDL_Renderer* renderer, Font* font, const char* text, int x, int y, SDL_Color color) {
	__Font_renderText(renderer, font, text, x,y, color, 0);
}
void Font_shadowText(SDL_Renderer* renderer, Font* font, const char* text, int x, int y, SDL_Color color) {
	__Font_renderText(renderer, font, text, x,y, color, 1);
}

// --------------------------------------------

void Fonts_init(void) {
	Font_init(font36);
	Font_init(font24);
	Font_init(font18);
	Font_init(font16);
}
void Fonts_quit(void) {
	Font_quit(font36);
	Font_quit(font24);
	Font_quit(font18);
	Font_quit(font16);
}

// --------------------------------------------
// custom menu
// --------------------------------------------

enum {
	SNAP_SAVE = 0,
	SNAP_CURRENT,
	SNAP_RESET,
};

static void App_screenshot(int game, int screen, int snap);
static void App_battery(int battery, int is_charging, int shadowed);
static  int App_capture(const char* path);
static void App_quit(void);

static void App_sync(int force) {
	if (force) app.synced = 0;
	
	if (app.synced) return;
	
	if (settings.cropped) {
		for (int i=0; i<SCREEN_COUNT; i++) {
			if (app.screens[i]) SDL_SetTextureScaleMode(app.screens[i], SDL_ScaleModeNearest);
			if (app.preview[i]) SDL_SetTextureScaleMode(app.preview[i], SDL_ScaleModeNearest);
		}
	}
	else {
		for (int i=0; i<SCREEN_COUNT; i++) {
			if (app.screens[i]) SDL_SetTextureScaleMode(app.screens[i], SDL_ScaleModeBest);
			if (app.preview[i]) SDL_SetTextureScaleMode(app.preview[i], SDL_ScaleModeBest);
		}
	}
	
	app.rects[0] = (SDL_Rect){0,0,256,192};
	app.rects[1] = (SDL_Rect){0,0,256,192};
	
	if (settings.cropped) {
		app.rects[0].w *= 2;
		app.rects[0].h *= 2;

		app.rects[1].w *= 2;
		app.rects[1].h *= 2;

		app.rects[0].x -= 8 * 2;
		app.rects[1].x -= 8 * 2;
	}
	else {
		app.rects[0].w = app.rects[1].w = 480;
		app.rects[0].h = app.rects[1].h = 360;
	}
	
	if (settings.spread) {
		app.rects[1].y = SCREEN_HEIGHT - app.rects[1].h;
	}
	else {
		app.rects[1].y = app.rects[0].h;
		int oy = (SCREEN_HEIGHT - app.rects[0].h*2) / 2;
		app.rects[0].y += oy;
		app.rects[1].y += oy;
	}
	
	// SDL_Log("screens[0]:{%i,%i,%i,%i} screens[1]:{%i,%i,%i,%i}",
	// 	app.rects[0].x, app.rects[0].y, app.rects[0].w, app.rects[0].h,
	// 	app.rects[1].x, app.rects[1].y, app.rects[1].w, app.rects[1].h
	// );
	
	app.synced = 1;
}
static void App_load(void) {
	drastic_load_state(0);
}
static void App_save(void) {
	App_screenshot(app.current, 0, SNAP_SAVE);
	App_screenshot(app.current, 1, SNAP_SAVE);
	drastic_save_state(0);
}
static void App_reset(void) {
	drastic_audio_pause(0);
	puts("[LOAD] reset requested");
	loader.state = LOADER_REQUESTED;
	loader.after = LOADER_RESET;
}

static int Device_OTG(void) {
    FILE *f = fopen("/sys/class/android_usb/android0/state", "r");
    if (!f) return 0;

    char buffer[64];
    int connected = fgets(buffer, sizeof(buffer), f) && strstr(buffer, "CONFIGURED");
    fclose(f);
	
    return connected;
}
static void Device_setLED(int enable) {
	raw_led(enable);
}
static void Device_mute(int mute) {
	if (mute) raw_vol(0);
	else Settings_setVolume(settings.volume);
}
static void Device_suspend(void) {
	// real_SDL_RenderClear(app.renderer);
	// real_SDL_RenderPresent(app.renderer);
	
	Settings_save();
	
	drastic_save_state(0);
	drastic_await_save();
	drastic_audio_pause(1);
	
	putString("/sys/power/state", "mem");
	
	drastic_audio_pause(0);
	Settings_setVolume(settings.volume);
}
static void Device_sleep(void) {
	raw_bri(0);
	raw_vol(0);
	putInt("/sys/class/graphics/fb0/blank", 4);
	Device_setLED(1);
	
	Settings_save();
	
	drastic_audio_pause(1);
	App_save();
	drastic_await_save();

	#define POWEROFF_TIMEOUT 2 * 60 * 1000 // two minutes
	int slept_at = SDL_GetTicks();
	int asleep = 1;
	SDL_Event event;
	while (asleep) {
		while (asleep && real_SDL_PollEvent(&event)) {
			if (event.type==SDL_KEYUP) {
				if (event.key.keysym.scancode==SCAN_POWER) {
					asleep = 0;
					break;
				}
			}
		}
		real_SDL_Delay(200);
		
		if (SDL_GetTicks()>=slept_at+POWEROFF_TIMEOUT) {
			int is_charging = getInt(app.usb);
			if (is_charging) {
				slept_at = SDL_GetTicks(); // keep on
			}
			else {
				unlink("/tmp/exec_loop");
				drastic_quit();
			}
		}
	}
	
	drastic_audio_pause(0);
	
	Settings_setVolume(settings.volume);
	Settings_setBrightness(settings.brightness);
	putInt("/sys/class/graphics/fb0/blank", 0);
	Device_setLED(0);
}
static void Device_goodbye(void) {
	if (!app.bg) app.bg = IMG_LoadTexture(app.renderer, ASSETS_PATH "/bg.png");
	real_SDL_RenderCopy(app.renderer, app.bg, NULL, NULL);
	
	Font_shadowText(app.renderer, font24, "Dedicated OS",	6, 6, LIGHT_COLOR);
	
	int battery = getInt(app.bat);
	int is_charging = getInt(app.usb);
	App_battery(battery,is_charging,1);	
	
	const char* lines[] = {
		"Saving &",
		"shutting",
		"down...",
	};
	for (int i=0; i<3; i++) {
		Font_shadowText(app.renderer, font36, lines[i], 6, 42+(i*36), WHITE_COLOR);
	}
	real_SDL_RenderPresent(app.renderer);
}
static void Device_poweroff(void) {
	Device_goodbye();
	Device_goodbye(); // backbuffer too :facepalm:
	
	// TODO: tmp
	// App_capture("/mnt/UDISK/capture.bmp");
	
	raw_vol(0);
	App_save();
	unlink("/tmp/exec_loop");
	Device_setLED(1);
	drastic_quit();
}
#define SLEEP_TIMEOUT (2 * 60 * 1000) // two minutes
#define WAKE_DEFER 250 // quarter of a second
#define POWER_TIMEOUT 1000 // one second
static int Device_handleEvent(SDL_Event* event) {
	static int menu_down = 0;
	static int menu_combo = 0;
	static int woken_at = 0;
	static int power_at = 0;
	
	// batmon manages power button itself
	if (!app.batmon) {
		if (power_at && SDL_GetTicks()-power_at>=POWER_TIMEOUT) {
			Device_poweroff();
		}
	
		if (event->type==SDL_KEYDOWN) {
			if (event->key.keysym.scancode==SCAN_POWER && event->key.repeat==0) {
				power_at = SDL_GetTicks();
				return 1;
			}
		}
		else if (event->type==SDL_KEYUP) {
			if (event->key.keysym.scancode==SCAN_POWER) {
				power_at = 0;
				if (SDL_GetTicks()-woken_at>WAKE_DEFER) {
					Device_sleep();
					woken_at = SDL_GetTicks();
				}
				return 1;
			}
		}
	}
	
	if (event->type==SDL_JOYBUTTONDOWN) {
		if (event->jbutton.button==JOY_MENU) {
			menu_down = 1;
			menu_combo = 0;
		}
		
		if (event->jbutton.button==JOY_SELECT) { // capture
			if (menu_down) {
				app.capture = 1;
				menu_combo = 1;
				return 1;
			}
		}
		
		if (event->jbutton.button==JOY_L2) {
			if (menu_down) {
				settings.cropped = !settings.cropped;
				App_sync(1);
				menu_combo = 1;
				return 1;
			}
		}
		else if (event->jbutton.button==JOY_R2) {
			if (menu_down) {
				settings.spread = !settings.spread;
				App_sync(1);
				menu_combo = 1;
				return 1;
			}
		}
		
		if (event->jbutton.button==JOY_PLUS) {
			if (settings.volume<20) {
				Settings_setVolume(settings.volume+1);
			}
			app.osd = OSD_VOLUME;
			osd_at = SDL_GetTicks();
			return 1;
		}
		else if (event->jbutton.button==JOY_MINUS) {
			if (settings.volume>0) {
				Settings_setVolume(settings.volume-1);
			}
			app.osd = OSD_VOLUME;
			osd_at = SDL_GetTicks();
			return 1;
		}
		
		if (event->jbutton.button==JOY_R1) {
			if (menu_down) {
				if (settings.brightness<10) {
					Settings_setBrightness(settings.brightness+1);
					menu_combo = 1;
				}
				app.osd = OSD_BRIGHTNESS;
				osd_at = SDL_GetTicks();
				return 1;
			}
		}
		else if (event->jbutton.button==JOY_L1) {
			if (menu_down) {
				if (settings.brightness>0) {
					Settings_setBrightness(settings.brightness-1);
					menu_combo = 1;
				}
				app.osd = OSD_BRIGHTNESS;
				osd_at = SDL_GetTicks();
				return 1;
			}
		}
	}
	else if (event->type==SDL_JOYBUTTONUP) {
		if (event->jbutton.button==JOY_MENU) {
			menu_down = 0;
			return menu_combo;
		}
	}
	
	return 0;
}

static void App_getDisplayName(const char* in_name, char* out_name) {
	char* tmp;
	char work_name[MAX_FILE];
	strcpy(work_name, in_name);
	strcpy(out_name, in_name);
	
	// extract just the filename if necessary
	tmp = strrchr(work_name, '/');
	if (tmp) strcpy(out_name, tmp+1);
	
	// remove extension(s), eg. .p8.png
	while ((tmp = strrchr(out_name, '.'))!=NULL) {
		int len = strlen(tmp);
		if (len>2 && len<=5) {
			if (tmp[1]==' ') tmp[1] = '\0'; // excempt . followed by whitespace
			else tmp[0] = '\0'; // 1-4 letter extension plus dot (was 1-3, extended for .doom files)
		}
		else break;
	}
	
	// remove trailing parens (round and square)
	strcpy(work_name, out_name);
	while ((tmp=strrchr(out_name, '('))!=NULL || (tmp=strrchr(out_name, '['))!=NULL) {
		if (tmp==out_name) break;
		tmp[0] = '\0';
		tmp = out_name;
	}
	
	// make sure we haven't nuked the entire name
	if (out_name[0]=='\0') strcpy(out_name, work_name);
	
	// remove trailing whitespace
	tmp = out_name + strlen(out_name) - 1;
	while(tmp>out_name && isspace((unsigned char)*tmp)) tmp--;
	tmp[1] = '\0';
}
static int App_sort(const void* a, const void* b) {
	const Entry *i = (const Entry *)a;
	const Entry *j = (const Entry *)b;
	return compareNatural(i->name, j->name);
}

static void App_empty(void) {
	SDL_Init(SDL_INIT_VIDEO);
	SDL_CreateWindow(NULL,0,0,SCREEN_WIDTH,SCREEN_HEIGHT,SDL_WINDOW_SHOWN);
	SDL_CreateRenderer(app.window, -1, SDL_RENDERER_ACCELERATED);
	
	if (!app.bg) app.bg = IMG_LoadTexture(app.renderer, ASSETS_PATH "/bg.png");
	
	real_SDL_RenderCopy(app.renderer, app.bg, NULL, NULL);

	Font_shadowText(app.renderer, font24, "Dedicated OS",	6, 6, LIGHT_COLOR);
	Font_shadowText(app.renderer, font36, "No games found", 6, 42, WHITE_COLOR);
	
	real_SDL_RenderPresent(app.renderer);
	
	sleep(5);
	unlink("/tmp/exec_loop");
	App_quit();
}
static void App_set(int i) {
	if (!app.count) App_empty();
	
	if (i>=app.count) i -= app.count;
	if (i<0) i += app.count;
	app.current = i;
	
	Entry* item = &app.items[app.current];
	strcpy(settings.game, item->name);
	
	static char game_path[MAX_PATH];
	sprintf(game_path, "%s/%s", item->hidden ? ARCHIVE_PATH : GAMES_PATH, settings.game);
	strcpy(app.game_path, game_path);
	
	strcpy(app.game_name, settings.game);
	char *dot = strrchr(app.game_name, '.');
	if (dot && dot!=app.game_name) *dot = '\0';
	
	SDL_Log("settings.game: %s", settings.game);
	SDL_Log("app.game_path: %s", app.game_path);
	SDL_Log("app.game_name: %s", app.game_name);
}

static void App_screenshot(int game, int screen, int snap) {
	if (!app.screens[screen]) return;
	SDL_Texture* texture = app.screens[screen];
	
	char game_name[MAX_FILE];
	if (snap==SNAP_CURRENT) strcpy(game_name, "current");
	else strcpy(game_name, app.items[game].name);
	char *dot = strrchr(game_name, '.');
	if (dot && dot!=game_name) *dot = '\0';
	
	char path[MAX_PATH];
	sprintf(path, USERDATA_PATH "/screenshots/%s-%i.bmp", game_name, screen);
	// SDL_Log("screenshot: %s", path);
	
	void *pixels = NULL;
	int pitch = 0;

	int w,h,access;
	Uint32 fmt;
	SDL_QueryTexture(texture, &fmt, &access, &w, &h);
	SDL_LockTexture(texture, NULL, &pixels, &pitch);

	const int depth = SDL_BITSPERPIXEL(fmt);
	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels, w, h, depth, pitch, fmt);
	if (surface) {
		SDL_SaveBMP(surface, path);
		SDL_FreeSurface(surface);
	}

	SDL_UnlockTexture(texture);
}
static void App_preview(int game, int screen, int snap) {
	if (!app.preview[screen]) return;
	SDL_Texture* texture = app.preview[screen];
	
	char game_name[MAX_FILE];
	if (snap==SNAP_CURRENT) strcpy(game_name, "current");
	else strcpy(game_name, app.items[game].name);
	char *dot = strrchr(game_name, '.');
	if (dot && dot!=game_name) *dot = '\0';
	
	char path[MAX_PATH];
	sprintf(path, USERDATA_PATH "/screenshots/%s-%i.bmp", game_name, screen);
	if (snap==SNAP_RESET || !exists(path)) sprintf(path, ASSETS_PATH "/screenshot-%i.png", screen);

	SDL_Log("preview: %s (snap: %i)", path, snap);
	SDL_Surface* tmp = IMG_Load(path);

	int w, h;
	Uint32 format;
	SDL_QueryTexture(texture, &format, NULL, &w, &h);

	if (format==tmp->format->format) {
		SDL_UpdateTexture(texture, NULL, tmp->pixels, tmp->pitch);
	} else {
		void *dst;
		int dst_pitch;
		if (SDL_LockTexture(texture, NULL, &dst, &dst_pitch)==0) {
			SDL_ConvertPixels(w, h, tmp->format->format, tmp->pixels, tmp->pitch, format, dst, dst_pitch);
			SDL_UnlockTexture(texture);
		}
	}
	
	SDL_FreeSurface(tmp);
}
static  int App_capture(const char* path) {
	app.capture = 0;
	SDL_Surface *tmp = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_WIDTH, SCREEN_HEIGHT, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_RenderReadPixels(app.renderer, NULL, SDL_PIXELFORMAT_ARGB8888, tmp->pixels, tmp->pitch);
	SDL_SaveBMP(tmp, path);
	SDL_FreeSurface(tmp);
}

static void App_init(void) {
	Settings_load();
	
	putString(CPU_PATH "scaling_governor", "userspace");
	putString(CPU_PATH "scaling_setspeed", FREQ_GAME);
	
	char* dirs[] = {
		GAMES_PATH,
		ARCHIVE_PATH,
	};
	
	app.count = 0;
	app.current = 0;
	app.capacity = 16;
	app.items = malloc(sizeof(Entry) * app.capacity);

	Fonts_init();
	
	// get games
	for (int i=0; i<NUMBER_OF(dirs); i++) {
		DIR* dir = opendir(dirs[i]);
		if (dir) {
			struct dirent* entry;
			while ((entry=readdir(dir))!=NULL) {
				if (entry->d_name[0]=='.') continue;
				if (entry->d_type==DT_DIR) continue;
			
				if (app.count>=app.capacity) {
					app.capacity *= 2;
					app.items = realloc(app.items, sizeof(Entry) * app.capacity);
				}
				Entry* item = &app.items[app.count++];
				item->hidden = i;
				snprintf(item->name, MAX_FILE, "%s", entry->d_name);
			}
			closedir(dir);
		}
	}
	
	if (app.count) qsort(app.items, app.count, sizeof(Entry), App_sort);
	
	// get index of last played game
	if (*settings.game) {
		for (int i=0; i<app.count; i++) {
			if (strcmp(app.items[i].name, settings.game)==0) {
				app.current = i;
				break;
			}
		}
	}
	
	App_set(app.current);
	
	app.bat = open(BAT_PATH "capacity", O_RDONLY);
	app.usb = open(USB_PATH "online", O_RDONLY);
	app.batmon = getInt(app.usb) && !Device_OTG();
}
static void App_quit(void) {
	SDL_Log("App_quit");
	
	free(app.items);
	
	if (app.bg) SDL_DestroyTexture(app.bg);
	if (app.eye) SDL_DestroyTexture(app.eye);
	if (app.overlay) SDL_DestroyTexture(app.overlay);
	if (app.preview[0]) SDL_DestroyTexture(app.preview[0]);
	if (app.preview[1]) SDL_DestroyTexture(app.preview[1]);
	
	close(app.bat);
	close(app.usb);
	
	Fonts_quit();
	
	Settings_save();
}
static int App_next(int start, int dir) {
	int i = start;
	int count = app.count;
	for (int _=0; _<count; _++) {
		i += dir;
		if (i>=count) i -= count;
		else if (i<0) i += count;
		if (!app.items[i].hidden || i==app.current) return i;
	}
	return start;
}
static void App_render(void) {
	if (!app.renderer) return;
	SDL_SetRenderDrawColor(app.renderer, BLACK_TRIAD,0xff);
	real_SDL_RenderClear(app.renderer);
	
	if (!app.screens[0] || !app.screens[1]) return;
	App_sync(0);
	for (int i=0; i<SCREEN_COUNT; i++) {
		real_SDL_RenderCopy(app.renderer, app.screens[i], NULL, &app.rects[i]);
	}
}
static int App_wrap(Font* font, char* text, int max_lines, char** lines, int* splits) {
	int line_count = 0;

	char line[1024] = {0};
	char word[256] = {0};
	char* p = text;
	int line_width = 0;
	
	while (*p) {
		char* start = p;
	
		// get next word
		while (*p && !isspace(*p)) p++;
		int word_len = p - start;
		strncpy(word, start, word_len);
		word[word_len] = '\0';
		
		// wrap on hyphen
		if (strcmp(word, "-")==0) {
			if (strlen(line)>0 && line_count<max_lines) {
				lines[line_count++] = strdup(line);
				splits[line_count] = 1;
				line[0] = '\0';
			}
			while (isspace(*p)) p++;
			continue;
		}
	
		// append to line
		char test_line[1024] = {0};
		if (strlen(line)>0) sprintf(test_line,"%s %s", line, word);
		else sprintf(test_line,"%s", word);
	
		Font_getTextSize(font, test_line, &line_width, NULL);
		line_width += font->offset_x * 2;
		
		int ow = 6 + 6;
		if (line_width<=SCREEN_WIDTH-ow) {
			strcpy(line, test_line);
		}
		else {
			if (line_count<max_lines) {
				lines[line_count++] = strdup(line);
			}
			else {
				line[0] = '\0';
				break;
			}
			strcpy(line, word);
		}
	
		// advance to next word
		while (isspace(*p)) p++;
	}

	// add trailing line
	if (strlen(line)>0 && line_count<max_lines) {
		lines[line_count++] = strdup(line);
	}
	return line_count;
}
static void App_trunc(Font* font, const char* text, int max_width, char* out_text) {
	int fw = 0;
	Font_getTextSize(font, text, &fw, NULL);
	
	if (fw<=max_width) {
		strcpy(out_text, text);
		return;
	}
	
	const char* sep = "...";
	int slen = strlen(sep);
	int sw = 0;
	Font_getTextSize(font, sep, &sw, NULL);
	// width(head) + width(sep) + width(tail) != width(head + sep + tail)
	// because it doesn't account for tracking/kerning against sep and head/tail
	// so we have to fudge it here or out_text could be wider than max_width
	sw += font->tracking * 2;
	
	fw = max_width - sw;
	int hw = fw / 2;
	int len = strlen(text);
	
	char tmp[MAX_FILE];
	int head = 0;
	int tail = 0;

	#define WORDS_ONLY 1 // looks much better to me
	
	// head
	for (int i=0; i<len; i++) {
		if (WORDS_ONLY && i!=0 && text[i]!=' ') continue;
		
		memcpy(tmp, text, i);
		tmp[i] = '\0';
		int w = 0;
		Font_getTextSize(font, tmp, &w, NULL);
		if (w>hw) {
			if (head) break; // found at least one word
			if (w>fw) {
				int j = i;
				while (j>0) {
					j -= 1;
					memcpy(tmp,text,j);
					tmp[j] = '\0';
					Font_getTextSize(font, tmp, &w, NULL);
					if (w<=fw) {
						head = j;
						break;
					}
				}
				break;
			}
			// else continue searching for a word break past the halfway
		}
		head = i;
	}
	
	// trim trailing space or dash
	while (WORDS_ONLY && head>0 && (text[head-1]==' ' || text[head-1]=='-')) head -= 1;
	
	memcpy(tmp, text, head);
	tmp[head] = '\0';
	int w = 0;
	Font_getTextSize(font, tmp, &w, NULL);

	hw = max_width - sw - w;
	if (hw<0) hw = 0;
	
	// tail
	for (int i=1; i<len-head; i++) {
		int start = len - i;
		if (WORDS_ONLY && start>0 && text[start-1]!=' ') continue;
		
		const char* trail = text + start;
		int w = 0;
		Font_getTextSize(font, trail, &w, NULL);
		if (w>hw) break;
		tail = i;
	}
	
	while (WORDS_ONLY && tail>0 && (text[len-tail]==' ' || text[len-tail]=='-')) tail -= 1;
	
	int i = 0;
	memcpy(out_text+i, text, head);
	i += head;
	memcpy(out_text+i, sep, slen);
	i += slen;
	if (tail) {
		memcpy(out_text+i, text+(len-tail), tail);
		i += tail;
	}
	out_text[i] = '\0';
}

// TODO: no longer antialiased, switch to images
static void AA_rect(int x, int y, int w, int h, int s, SDL_Color c) {
	if (s==0) {
		// body
		SDL_SetRenderDrawColor(app.renderer, c.r,c.g,c.b,c.a);
		const SDL_Rect rects[] = {
			{x + 0, y+1,   1, h-2}, // left
			{x + 1, y+0, w-2, h  }, // middle
			{x+w-1, y+1,   1, h-2}, // right
		};
		SDL_RenderFillRects(app.renderer, rects, NUMBER_OF(rects));
	}
	else {
		// outline
		SDL_SetRenderDrawColor(app.renderer, c.r,c.g,c.b,c.a);
		const SDL_Rect rects[] = {
			{x + 0, y + 1,   1, h-2}, // left
			{x + 1, y + 0, s-1, h  }, // left
			{x + s, y + 0, w-8, s  }, // top
			{x + s, y+h-s, w-8, s  }, // bottom
			{x+w-s, y + 0, s-1, h  }, // right
			{x+w-1, y + 1,   1, h-2}, // right
		};
		SDL_RenderFillRects(app.renderer, rects, NUMBER_OF(rects));
	}
}
static void AA_bolt(int x, int y, SDL_Color c) {
	const SDL_Rect rects[] = {
		// top left
		{x+ 6,y+ 0, 6,2},
		{x+ 5,y+ 2, 6,2},
		{x+ 4,y+ 4, 6,2},
		{x+ 3,y+ 6, 6,2},
		// middle
		{x+ 9,y+ 7,11,1},
		{x+ 2,y+ 8,17,2},
		{x+ 1,y+10,17,2},
		{x+ 0,y+12,11,1},
		// bottom right
		{x+11,y+12, 6,2},
		{x+10,y+14, 6,2},
		{x+ 9,y+16, 6,2},
		{x+ 8,y+18, 6,2},
	};
	SDL_SetRenderDrawColor(app.renderer, c.r,c.g,c.b,c.a);
	SDL_RenderFillRects(app.renderer, rects, NUMBER_OF(rects));
}
static void AA_bat(int x, int y, int battery, SDL_Color c) {
	AA_rect(x,y,48,28, 4, c);
	AA_rect(x+47,y+8,5,12, 0, c);
	
	int w = CEIL_TO(battery,20) * 30 / 100;
	if (w>0) AA_rect(x+8,y+8,w+2,12, 0, c);
}

static void App_battery(int battery, int is_charging, int shadowed) {
	int x = 426;
	int y = 2;
	
	SDL_Color c = LIGHT_COLOR;

	if (shadowed) AA_bat(x+2,y+2, battery, BLACK_COLOR);
	AA_bat(x,y, battery, c);

	if (is_charging && battery<100) {
		y = 6;
		
		char percent[8];
		sprintf(percent, "%i%%", battery);
		int w = 0;
		Font_getTextSize(font24, percent, &w, NULL);
		
		x -= w + 4;
		Font_shadowText(app.renderer, font24, percent, x,y, c);

		x -= 24;
		AA_bolt(x+2,y+2,BLACK_COLOR);
		AA_bolt(x,y,c);
	}
}

static int in_drastic_menu = 0; // TODO: only used for debug, and I seem to have broken it at some point :cold_sweat:

static void App_OSD(char* label, int value, int max) {
	LOG("OSD %s", label);
	
	int nh = 28;
	int x,y,w,h;
	Font_getTextSize(font16, label, &w, &h);
	w = 252;
	h = 64;
	x = (SCREEN_WIDTH - w) / 2;
	SDL_Rect* rect = &app.rects[0];
	y = rect->y + rect->h - h - 32;
	
	AA_rect(x,y,w,h, 0, TRIAD_ALPHA(BLACK_TRIAD,0x60));

	Font_shadowText(app.renderer, font16, label, x+8, y+8, LIGHT_COLOR);
	
	int nw = max==10?20:8;
	int no = max==10?24:12;
	for (int i=0; i<max; i++) {
		int nx = x + 8;
		int ny = y + 28;
		AA_rect(nx+i*no+2,ny+2,nw,nh, 0, BLACK_COLOR);
		if (i<value) {
			AA_rect(nx+i*no,ny,nw,nh, 0, WHITE_COLOR);
		}
	}
}

static void App_menu(void) {
	SDL_Log("enter menu");

	Device_mute(1);

	app.menu = 1;
	putString(CPU_PATH "scaling_setspeed", FREQ_MENU);
	
	static int last_battery = 0;
	static int was_charging = 0;
	
	int current = app.current;
	App_screenshot(current, 0, SNAP_CURRENT);
	App_screenshot(current, 1, SNAP_CURRENT);
	
	SDL_Surface* tmp;
	if (!app.overlay) {
		int w = 1; // we can just stretch horizontally on the GPU
		int h = SCREEN_HEIGHT;
		tmp = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);

		uint32_t* d = tmp->pixels;
		int total = w * h;
		for (int i=0; i<total; i++,d++) {
			*d = (64 + ((total - i) * 160 / total)) << 24; // 87.5% to 25%
		}

		app.overlay = SDL_CreateTextureFromSurface(app.renderer, tmp);
		SDL_SetTextureBlendMode(app.overlay, SDL_BLENDMODE_BLEND);
		SDL_FreeSurface(tmp);
	}
	
	if (!app.preview[0]) {
		app.preview[0] = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256,192);
		app.preview[1] = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256,192);
	}
	
	if (!app.eye) {
		app.eye = IMG_LoadTexture(app.renderer, ASSETS_PATH "/icon-eye.png");
	}
	
	char* save_items[] = {
		"SAVE",
		"LOAD",
		"ARCHIVE",
		"RESET",
	};
	char* load_items[] = {
		"LOAD",
		"ARCHIVE",
	};
	
	int menu_at = SDL_GetTicks();
	int selected = 0;
	int dirty = 1;
	int top = 0;
	int rows = 12;
	int in_menu = 1;
	SDL_Event event;
	int mode = MODE_MENU;
	int last_osd = app.osd;
	while (in_menu) {
		
		if (osd_at+1000<SDL_GetTicks()) app.osd = OSD_NONE;
		if (last_osd!=app.osd) {
			last_osd = app.osd;
			dirty = 1;
		}

		while (in_menu && Repeater_pollEvent(&event)) {
			// LOG_event(&event);
			int btn = event.jbutton.button;
			
			if (Device_handleEvent(&event)) {
				dirty = 1;
				continue;
			}
			
			if (event.type==SDL_JOYBUTTONDOWN) {
				menu_at = SDL_GetTicks();
				
				if (mode==MODE_MENU) {
					if (btn==JOY_UP) {
						selected -= 1;
						dirty = 1;
					}
					else if (btn==JOY_DOWN) {
						selected += 1;
						dirty = 1;
					}
				
					if (btn==JOY_RIGHT) {
						selected = 0;
						current = App_next(current, +1);
						dirty = 1;
					}
					else if (btn==JOY_LEFT) {
						selected = 0;
						current = App_next(current, -1);
						dirty = 1;
					}
				
					if (btn==JOY_B) { // BACK
						if (current!=app.current) {
							current = app.current;
							selected = 0;
							dirty = 1;
						}
						else {
							in_menu = 0;
						}
					}
					else if (btn==JOY_A) { // SELECT
						if (current==app.current) {
							if (selected==0) { // SAVE
								App_save();
							}
							else if (selected==1) { // LOAD
								App_load();
							}
							else if (selected==2) { // ARCHIVE
								mode = MODE_ARCHIVE;
								selected = current;
								dirty = 1;
							}
							else if (selected==3) { // RESET
								App_reset();
							}
							if (mode==MODE_MENU) in_menu = 0;
						}
						else {
							if (selected==0) { // LOAD
								App_save();
								App_set(current);
								Settings_save();
							
								drastic_audio_pause(0);
								puts("[LOAD] resume requested");
								loader.state = LOADER_REQUESTED;
								loader.after = LOADER_RESUME;
							
								in_menu = 0;
							}
							else if (selected==1) { // ARCHIVE
								mode = MODE_ARCHIVE;
								selected = current;
								dirty = 1;
							}
						}
					}
				}
				else if (mode==MODE_ARCHIVE) {
					if (btn==JOY_UP) { // ROW UP
						current -= 1;
						if (current<0) current += app.count;
						dirty = 1;
					}
					else if (btn==JOY_DOWN) { // ROW DOWN
						current += 1;
						if (current>=app.count) current -= app.count;
						dirty = 1;
					}
					
					if (btn==JOY_RIGHT) { // PAGE FORWARD
						if (current==app.count-1) current = 0;
						else {
							current += rows;
							if (current>=app.count) current = app.count-1;
						}
						dirty = 1;
					}
					else if (btn==JOY_LEFT) { // PAGE BACK
						if (current==0) current = app.count-1;
						else {
							current -= rows;
							if (current<0) current = 0;
						}
						dirty = 1;
					}
					
					if (btn==JOY_A) { // TOGGLE
						Entry* item = &app.items[current];
						
						char shown[MAX_FILE];
						sprintf(shown, "%s/%s", GAMES_PATH, item->name);
						char hidden[MAX_FILE];
						sprintf(hidden, "%s/%s", ARCHIVE_PATH, item->name);
						
						char *dst,*src;
						if (item->hidden) {
							src = hidden;
							dst = shown;
						}
						else {
							src = shown;
							dst = hidden;
						}
						
						if (!exists(dst) && rename(src, dst)==0) {
							item->hidden = !item->hidden;
							dirty = 1;
						}
					}
					if (btn==JOY_B) { // BACK
						mode = MODE_MENU;
						dirty = 1;
						// reusing unused selected to store current
						// upon entering archive so we can restore
						// when existing, selected will always
						// revert to ARCHIVE
						current = selected;
						selected = current==app.current ? 2 : 1;
					}
				}
				
				if (btn==JOY_START) { // TODO: tmp
					drastic_save_state(0);
					drastic_quit();
					in_menu = 0;
				}
			}
			else if (event.type==SDL_JOYBUTTONUP) {
				menu_at = SDL_GetTicks();
				if (btn==JOY_MENU) {
					in_menu = 0;
				}
			}
		}
		
		int is_charging = getInt(app.usb);
		if (is_charging!=was_charging) {
			was_charging = is_charging;
			dirty = 1;
		}
		
		int battery = getInt(app.bat);
		if (battery!=last_battery) {
			last_battery = battery;
			dirty = 1;
		}
		
		// let the hook position them (for now)
		if (dirty) {
			dirty = 0;
			
			char** items;
			int count;
			if (current==app.current) {
				items = save_items;
				count = NUMBER_OF(save_items);
			}
			else {
				items = load_items;
				count = NUMBER_OF(load_items);
			}
			
			// TODO: BAD this is update logic in render block
			if (mode==MODE_MENU) {
				if (selected<0) selected += count;
				selected %= count;
			}
			
			// TODO: what do we display for ARCHIVE?
			int snap = selected==3 ? SNAP_RESET : (current==app.current && selected==0 ? SNAP_CURRENT : SNAP_SAVE);
			App_preview(current,0, snap);
			App_preview(current,1, snap);
			
			// screens and gradient
			SDL_SetRenderDrawColor(app.renderer, BLACK_TRIAD,0xff);
			real_SDL_RenderClear(app.renderer);
	
			App_sync(1);
			for (int i=0; i<SCREEN_COUNT; i++) {
				real_SDL_RenderCopy(app.renderer, app.preview[i], NULL, &app.rects[i]);
				if (mode==MODE_ARCHIVE) break; // only draw top screen
			}
			
			real_SDL_RenderCopy(app.renderer, app.overlay, NULL, NULL);
			
			// system
			Font_shadowText(app.renderer, font24, "Nintendo DS", 6,6, LIGHT_COLOR);

			int x,y,w,h;
			
			// battery
			App_battery(battery,is_charging,1);
			
			// game name
			char name[MAX_FILE];
			App_getDisplayName(app.items[current].name, name);
	
			#define MAX_LINES 8
			char* lines[MAX_LINES];
			int splits[MAX_LINES] = {0};
			int line_count = App_wrap(font36, name, MAX_LINES, lines, splits);
			SDL_Color color = WHITE_COLOR;
			for (int i=0; i<line_count; i++) {
				if (splits[i]) color = LIGHT_COLOR;
				Font_shadowText(app.renderer, font36, lines[i], 6, 42+(i*36), color);
				free(lines[i]);
			}
			
			if (mode==MODE_MENU) {
				// center in bottom "screen"
				y = h = SCREEN_HEIGHT / 2;
			
				int mw = 0;
				for (int i=0; i<count; i++) {
					Font_getTextSize(font24, items[i], &w, NULL);
					if (w>mw) mw = w;
				}
			
				w = 8 + mw + 8;
				x = (SCREEN_WIDTH - w) / 2;
			
				int oh = ((count-1) * 40) + 36;
				y += (h - oh) / 2;
				h = oh;
				oh = 40;
			
				AA_rect(x-8,y-8,8+w+8,8+h+8, 0, TRIAD_ALPHA(BLACK_TRIAD,0x40));
			
				for (int i=0; i<count; i++) {
					Font_renderFunc renderer = Font_shadowText;
					SDL_Color color = WHITE_COLOR;
				
					if (i==selected) {
						AA_rect(x+2,y+(i*oh)+2,w,36, 0, TRIAD_ALPHA(BLACK_TRIAD,0xff));
						AA_rect(x,y+(i*oh),w,36, 0, TRIAD_ALPHA(WHITE_TRIAD,0xff));
						renderer = Font_renderText;
						color = DARK_COLOR;
					}
				
					renderer(app.renderer, font24, items[i], x+8, y+8+(i*oh), color);
				}
			}
			else if (mode==MODE_ARCHIVE) {
				// calculate viewport
				if (app.count<=rows) {
					top = 0;
				}
				else {
					int max = app.count - rows;
					int bottom = top + rows - 1;
					if (current<top) top = current;
					else if (current>bottom) top = current - (rows - 1);
					if (top<0) top = 0;
					if (top>max) top = max;
				}
				int end = top + rows;
				if (end>app.count) end = app.count;
				
				x = 0;
				y = (SCREEN_HEIGHT / 2) + 16;
				h = 32;
				
				// draw viewport
				for (int i=top; i<end; i++) {
					int row = i - top;
					int oy = y + row * h;
					
					Entry* item = &app.items[i];
					SDL_Color c = item->hidden ? LIGHT_COLOR : WHITE_COLOR;
					if (i==current) {
						AA_rect(x,oy,SCREEN_WIDTH,h, 0, c);
						c = BLACK_COLOR;
					}
					
					char fit[MAX_FILE];
					App_getDisplayName(item->name, name);
					App_trunc(font18, name, 440, fit);
					
					real_SDL_RenderCopy(app.renderer, app.eye, &(SDL_Rect){item->hidden?24:0,0,24,24}, &(SDL_Rect){x+4,oy+4,24,24});
					Font_renderText(app.renderer, font18, fit,	x+32,oy+9, c);
				}
			}
			
			// volume/brightness osd
			if (app.osd==OSD_VOLUME) App_OSD("VOLUME", settings.volume, 20);
			else if (app.osd==OSD_BRIGHTNESS) App_OSD("BRIGHTNESS", settings.brightness, 10);
			
			// flip
			real_SDL_RenderPresent(app.renderer);
			
			if (app.capture) App_capture("/tmp/capture.bmp");
		}
		else {
			real_SDL_Delay(16);
		}
		
		#define MENU_TIMEOUT 30 * 1000 // thirty seconds
		if (SDL_GetTicks()>=menu_at+MENU_TIMEOUT) {
			if (!is_charging) {
				Device_sleep();
				dirty = 1;
			}
			menu_at = SDL_GetTicks();
		}
	}
	
	putString(CPU_PATH "scaling_setspeed", FREQ_GAME);
	if (!app.fast_forward && loader.state==LOADER_IDLE) Device_mute(0);
	app.menu = 0;

	SDL_Log("exit menu");
}

#define BATMON_TIMEOUT (5 * 1000) // five seconds
static void App_batmon(void) {
	SDL_Log("batmon");
	
	if (!app.bg) app.bg = IMG_LoadTexture(app.renderer, ASSETS_PATH "/bg.png");
	
	putString(CPU_PATH "scaling_setspeed", FREQ_MENU);
	
	static int last_battery = 0;
	static int was_charging = 0;
	
	int asleep = 0;
	int wake = 0;
	int power_off = 0;
	int power_at = 0;
	int input_down_at = SDL_GetTicks();
	int dirty = 1;
	int last_osd = app.osd;
	while (app.batmon) {

		if (osd_at+1000<SDL_GetTicks()) app.osd = OSD_NONE;
		if (last_osd!=app.osd) {
			last_osd = app.osd;
			dirty = 1;
		}
		
		SDL_Event event;
		while (app.batmon && Repeater_pollEvent(&event)) {
			int btn = event.jbutton.button;
			
			if (Device_handleEvent(&event)) {
				dirty = 1;
				continue;
			}	

			if (power_at && SDL_GetTicks()-power_at>=POWER_TIMEOUT) {
				power_off = 1;
			}
	
			if (event.type==SDL_KEYDOWN) {
				if (event.key.keysym.scancode==SCAN_POWER && event.key.repeat==0) {
					power_at = SDL_GetTicks();
				}
			}
			else if (event.type==SDL_KEYUP) {
				if (event.key.keysym.scancode==SCAN_POWER) {
					app.batmon = 0;
					if (asleep) wake = 1;
				}
			}
			
			// NOTE: touches don't register when the screen is off
			if (event.type==SDL_JOYBUTTONDOWN || event.type==SDL_FINGERDOWN) {
				input_down_at = SDL_GetTicks();
				if (asleep) wake = 1; 
			}
		}
		
		int battery = getInt(app.bat);
		if (battery!=last_battery) {
			last_battery = battery;
			dirty = 1;
		}
		
		int is_charging = getInt(app.usb);
		if (is_charging!=was_charging) {
			was_charging = is_charging;
			dirty = 1;
			
			if (!is_charging) power_off = 1;
		}
		
		if (power_off) {
			fflush(stdout);
			SDL_Log("power off");
			
			// power off on unplug
			unlink("/tmp/exec_loop");
			Device_setLED(1);
			exit(0);
		}
		
		if (!wake && SDL_GetTicks()-input_down_at>=BATMON_TIMEOUT) {
			asleep = 1;
			raw_bri(0);
			putInt("/sys/class/graphics/fb0/blank", 4);
			Device_setLED(1);
		}

		if (wake) {
			wake = 0;
			Settings_setBrightness(settings.brightness);
			putInt("/sys/class/graphics/fb0/blank", 0);
			Device_setLED(0);
			asleep = 0;
		}
		
		if (!app.batmon) {
			// blank screen before returning
			for (int i=0; i<2; i++) {
				SDL_SetRenderDrawColor(app.renderer, BLACK_TRIAD,0xff);
				real_SDL_RenderClear(app.renderer);
				real_SDL_RenderPresent(app.renderer);
			}
			break;
		}
			
		if (dirty) {
			dirty = 0;
			
			SDL_SetRenderDrawColor(app.renderer, BLACK_TRIAD,0xff);
			real_SDL_RenderClear(app.renderer);

			App_sync(1);

			real_SDL_RenderCopy(app.renderer, app.bg, NULL, NULL);
			
			Font_shadowText(app.renderer, font24, "Dedicated OS", 6,6, LIGHT_COLOR);
			Font_shadowText(app.renderer, font36, is_charging && battery<100 ? "Charging..." : "Fully charged", 6,42, WHITE_COLOR);

			App_battery(battery,is_charging,1);
			
			int x = 116;
			int y = 526;
			Font_shadowText(app.renderer, font24, "Press Power", x,y, WHITE_COLOR); y += 28;
			Font_renderText(app.renderer, font18, "again to play", x,y, LIGHT_COLOR); y += 44;

			Font_shadowText(app.renderer, font24, "Hold Power", x,y, WHITE_COLOR); y += 28;
			Font_renderText(app.renderer, font18, "to shut down", x,y, LIGHT_COLOR); y += 24;
			Font_renderText(app.renderer, font18, "and continue", x,y, LIGHT_COLOR); y += 24;
			Font_renderText(app.renderer, font18, "charging", x,y, LIGHT_COLOR);
			
			// volume/brightness osd
			if (app.osd==OSD_VOLUME) App_OSD("VOLUME", settings.volume, 20);
			else if (app.osd==OSD_BRIGHTNESS) App_OSD("BRIGHTNESS", settings.brightness, 10);
			
			// flip
			real_SDL_RenderPresent(app.renderer);
			
			if (app.capture) App_capture("/tmp/capture.bmp");
		}
		else {
			real_SDL_Delay(16);
		}
	}
	
	putString(CPU_PATH "scaling_setspeed", FREQ_GAME);
	SDL_Log("exit batmon");
}

// --------------------------------------------
// hook SDL
// --------------------------------------------

int SDL_Init(Uint32 flags) {
	Settings_setBrightness(settings.brightness);
	return real_SDL_Init(flags);
}
SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags) {
	app.window = real_SDL_CreateWindow(title, x, y, w, h, flags); // window size is always 480x800
	return app.window;
}
void SDL_SetWindowSize(SDL_Window* window, int w, int h) {
	// window size is always 480x800
	// real_SDL_SetWindowSize(window, w, h); 
}
int SDL_PollEvent(SDL_Event* event) {
	
	// loop is required to capture button presses
	while (1) {
		int result = Repeater_pollEvent(event);
		if (!result) return 0;
		
		if (Device_handleEvent(event)) continue;
		
		if (event->type==SDL_JOYBUTTONDOWN) {
			if (event->jbutton.button==JOY_L3) {
				app.fast_forward = !app.fast_forward;
				if (app.fast_forward) Device_mute(1);
				else Device_mute(0);
			}
			if (event->jbutton.button==JOY_MENU) {
				// in_drastic_menu = !in_drastic_menu; // TODO: uncomment to enable drastic menu (hacky)
				continue;
			}
		}
		else if (event->type==SDL_JOYBUTTONUP) {
			if (event->jbutton.button==JOY_MENU) {
				if (!in_drastic_menu) {
					App_menu();
					continue;
				}
			}
		}
		
		switch (event->type) {
			case SDL_FINGERDOWN:
			case SDL_FINGERUP:
			case SDL_FINGERMOTION: {
				// for some reason drastic isn't expecting 
				// normalized coords so convert to window
				int x = event->tfinger.x * SCREEN_WIDTH;
				int y = event->tfinger.y * SCREEN_HEIGHT;
				
				// SDL_Log("touch (window) %i,%i (%f,%f)", x,y, event->tfinger.x,event->tfinger.y);

				SDL_Rect src = app.rects[1];
				
				if (y<400) continue; // ignore area outside bottom screen
				
				if (y<src.y) y = src.y;
				else if (y>=src.y+src.h) y = src.y+src.h-1;

				static const SDL_Rect dst = { 0, 400, 480, 400 };

				int dx = x - src.x;
				int dy = y - src.y;
				x = dst.x + ((dx * dst.w + (src.w >> 1)) / src.w);
				y = dst.y + ((dy * dst.h + (src.h >> 1)) / src.h);

				// SDL_Log("touch (adjust) %i,%i", x,y);
				event->tfinger.x = x;
				event->tfinger.y = y;
			} break;
		}
		
		return result;
	}
}

int SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h) {
	return 1; // complete render takeover
}
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture  *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect) {
	if (in_drastic_menu) {
		int tw,th;
		SDL_QueryTexture(texture, NULL, NULL, &tw, &th);
		SDL_Log("texture %ix%i", tw, th);
	}
	
	if (in_drastic_menu) return real_SDL_RenderCopy(renderer, texture, srcrect, dstrect);
	return 1; // complete render takeover
}

SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, Uint32 flags) {
	app.renderer = real_SDL_CreateRenderer(window, index, flags);
	if (app.batmon) App_batmon();
	return app.renderer;
}
int SDL_RenderClear(SDL_Renderer* renderer) {
	if (in_drastic_menu) return real_SDL_RenderClear(renderer);
	return 1; // complete render takeover
}
void SDL_RenderPresent(SDL_Renderer * renderer) {
	// puts("SDL_RenderPresent"); fflush(stdout);
	if (!in_drastic_menu) {
		if (loader.state!=LOADER_IDLE) {
			if (loader.state==LOADER_REQUESTED) {
				puts("[LOAD] requesting load...");
				loader.state = LOADER_AWAITING;
				drastic_load_nds_and_jump(app.game_path);
				return;
			}
			
			if (loader.state==LOADER_COMPLETE) {
				puts("[LOAD] load complete");
				loader.state = LOADER_IDLE;
				if (loader.after==LOADER_RESUME) {
					puts("[LOAD] perform resume");
					loader.after = LOADER_RESET;
					drastic_load_state(0);
				}
				else puts("[LOAD] perform reset");
				Device_mute(0);
			}
			
			return;
		}
		
		App_render(); // complete render takeover
	}
	
	if (!app.menu) {
		int battery = getInt(app.bat);
		int is_charging = getInt(app.usb);
		if (battery<=10 && !is_charging) App_battery(battery,0,0);
		
		if (app.osd==OSD_VOLUME) App_OSD("VOLUME", settings.volume, 20);
		else if (app.osd==OSD_BRIGHTNESS) App_OSD("BRIGHTNESS", settings.brightness, 10);
	} 
	real_SDL_RenderPresent(renderer);
	
	if (app.capture) App_capture("/tmp/capture.bmp");

	if (osd_at+1000<SDL_GetTicks()) app.osd = OSD_NONE;
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int type, int w, int h) {
	SDL_Texture *texture = real_SDL_CreateTexture(renderer, format, type, w, h);
	if (type==SDL_TEXTUREACCESS_STREAMING) {
		if (w==256 && h==192) {
			if (!app.screens[0]) app.screens[0] = texture;
			else if (!app.screens[1]) app.screens[1] = texture;
		}
	}
	return texture;
}
void SDL_DestroyTexture(SDL_Texture *texture) {
	if (texture==app.screens[0]) app.screens[0] = NULL;
	else if (texture==app.screens[1]) app.screens[1] = NULL;
	
	real_SDL_DestroyTexture(texture);
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) {
	int result = real_SDL_OpenAudio(desired, obtained);
	Settings_setVolume(settings.volume); // must be done here (or later)
	return result;
}

void SDL_Delay(uint32_t ms) {
	if (ms>16) {
		SDL_Log("excessive SDL_Delay(%i)!", ms);
		ms = 16;
	}
	real_SDL_Delay(ms);
}

// --------------------------------------------
// logging and string formatting hooks
// --------------------------------------------

// #define DISABLE_LOGGING

int __printf_chk(int flag, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	
	// listen for loading events
	if (loader.state==LOADER_AWAITING && strncmp(fmt, "Gamecard title", 14)==0) {
		puts("[LOAD] load started");
		loader.state = LOADER_STARTED;
	}
	else if (loader.state==LOADER_STARTED && strncmp(fmt, "Remapping DTCM", 14)==0) {
		puts("[LOAD] load completed");
		loader.state = LOADER_COMPLETE;
	}

	// silence spam
	if (strncmp(fmt, "vf ticks", 8)==0 || strncmp(fmt, "ticks_delta", 11)==0) {
		va_end(ap);
		return 0;
	}
	
#ifdef DISABLE_LOGGING
	va_end(ap);
	return 0;
#endif
	
	char tick_fmt[1024];
	snprintf(tick_fmt, sizeof tick_fmt, "[%i] %s", SDL_GetTicks(), fmt);

	int r = __vprintf_chk(flag, tick_fmt, ap);
	va_end(ap);
	return r;

	return 0;
}
int puts (const char *msg) {
#ifdef DISABLE_LOGGING
	return 0;
#endif

	char tick_msg[1024];
	snprintf(tick_msg, sizeof tick_msg, "[%i] %s", SDL_GetTicks(), msg);
	return real_puts(tick_msg);
}

__attribute__((noreturn))
void exit(int status) {
	App_quit();
	real_exit(status);
}
__attribute__((noreturn))
void _exit(int status) {
	App_quit();
	real__exit(status);
}
int system(const char *command) {
	unsetenv("LD_PRELOAD");
	return real_system(command);
}

int __snprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...) {
	// hooked to repath save data
	if (fmt && strcmp(fmt, "%s%cbackup%c%s.dsv")==0) return real__snprintf_chk(s, maxlen, flag, slen, USERDATA_PATH "/saves/%s.sram", app.game_name);

	va_list ap;
	va_start(ap, fmt);
	int r = __vsnprintf_chk(s, maxlen, flag, slen, fmt, ap);
	va_end(ap);
	return r;
}
int __sprintf_chk(char *s, int flag, size_t slen, const char *fmt, ...) {
	// hooked to repath official bios (but not clean room bios)
	if (fmt && strcmp(fmt, "%s%csystem%c%s")==0)  {
		va_list ap;
		va_start(ap, fmt);
		va_arg(ap, const char*); va_arg(ap, int); va_arg(ap, int); // skip %s %c %c
		const char* bios = va_arg(ap, const char*);	// %s
		va_end(ap);
		
		if (bios && strncmp(bios, "nds_",4)==0) return real__sprintf_chk(s, flag, slen, SDCARD_PATH "/bios/%s", bios);
	}

	va_list ap;
	va_start(ap, fmt);
	int r = __vsprintf_chk(s, flag, slen, fmt, ap);
	va_end(ap);
	return r;
}

// --------------------------------------------
// hijack main to modify args
// --------------------------------------------

static int pick_main(struct dl_phdr_info *i, size_t s, void *out) {
	(void)s;
	if (!i->dlpi_name || !i->dlpi_name[0]) {
		*(uintptr_t *)out = (uintptr_t)i->dlpi_addr;
		return 1;
	}
	return 0;
}
static inline uintptr_t find_exe_base(void) {
	uintptr_t base = 0;
	dl_iterate_phdr(pick_main, &base);
	return base;
}

static void resolve_real(void) {
	// hook glibc? functions
	real__libc_start_main = dlsym(RTLD_NEXT, "__libc_start_main");
	real__sprintf_chk = dlsym(RTLD_NEXT, "__sprintf_chk");
	real__snprintf_chk = dlsym(RTLD_NEXT, "__snprintf_chk");
	real_exit  = dlsym(RTLD_NEXT, "exit");
	real__exit = dlsym(RTLD_NEXT, "_exit");
	real_system = dlsym(RTLD_NEXT, "system");
	real_puts = dlsym(RTLD_NEXT, "puts");
	real__printf_chk = dlsym(RTLD_NEXT, "__printf_chk");
	
	// hook SDL functions
	real_SDL_Init = dlsym(RTLD_NEXT, "SDL_Init");
	
	real_SDL_CreateWindow = dlsym(RTLD_NEXT, "SDL_CreateWindow");
	real_SDL_SetWindowSize = dlsym(RTLD_NEXT, "SDL_SetWindowSize");
	
	real_SDL_PollEvent = dlsym(RTLD_NEXT, "SDL_PollEvent");
	
	real_SDL_CreateRenderer = dlsym(RTLD_NEXT, "SDL_CreateRenderer");
	real_SDL_RenderSetLogicalSize = dlsym(RTLD_NEXT, "SDL_RenderSetLogicalSize");
	real_SDL_RenderClear = dlsym(RTLD_NEXT, "SDL_RenderClear");
	real_SDL_RenderCopy = dlsym(RTLD_NEXT, "SDL_RenderCopy");
	real_SDL_RenderPresent = dlsym(RTLD_NEXT, "SDL_RenderPresent");
	
	real_SDL_CreateTexture = dlsym(RTLD_NEXT, "SDL_CreateTexture");
	real_SDL_DestroyTexture = dlsym(RTLD_NEXT, "SDL_DestroyTexture");
	
	real_SDL_OpenAudio = dlsym(RTLD_NEXT, "SDL_OpenAudio");
	real_SDL_Delay = dlsym(RTLD_NEXT, "SDL_Delay");
	
	app.base = find_exe_base();
	
	printf("resolved real function pointers\n"); fflush(stdout);
}

static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static inline void hook(void) {
	pthread_once(&resolve_once, resolve_real);
}

static drastic_main_t drastic_main = NULL;
static int override_args(int argc, char **argv, char **envp) {
	hook();
	App_init();
	char *new_argv[] = { argv[0], app.game_path, NULL };
	
	puts("[LOAD] resume requested");
	return drastic_main(2, new_argv, envp);
}

int __libc_start_main(int (*main)(int,char**,char**), int argc, char **ubp_av, void (*init)(void), void (*fini)(void), void (*rtld_fini)(void), void *stack_end) {
	hook();
	drastic_main = main;
	return real__libc_start_main(override_args, argc, ubp_av, init, fini, rtld_fini, stack_end);
}