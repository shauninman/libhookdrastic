#define _GNU_SOURCE // for RTLD_NEXT
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <setjmp.h>
#include <dirent.h>

#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

// --------------------------------------------

#define CEIL_TO(x, n) (((x) + (n) - 1) / (n) * (n))
#define NUMBER_OF(items) (sizeof(items) / sizeof((items)[0]))

#define SDCARD_PATH		"/mnt/SDCARD"
#define GAMES_PATH 		SDCARD_PATH "/games"
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

#define FAUX_BOLT "͛"
#define BUTTON_BG "⬤"

#define BLACK_TRIAD 	0x00,0x00,0x00
#define WHITE_TRIAD 	0xff,0xff,0xff
#define RED_TRIAD		0xff,0x33,0x33
#define GREEN_TRIAD		0x33,0xcc,0x33
#define YELLOW_TRIAD	0xff,0xcc,0x00

#define BLACK_COLOR (SDL_Color){BLACK_TRIAD}
#define WHITE_COLOR (SDL_Color){WHITE_TRIAD}
#define RED_COLOR (SDL_Color){RED_TRIAD}
#define GREEN_COLOR (SDL_Color){GREEN_TRIAD}
#define YELLOW_COLOR (SDL_Color){YELLOW_TRIAD}

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

#define JOY_VOLUP	17
#define JOY_VOLDN	16

#define SCAN_POWER	102

// --------------------------------------------

static struct {
	uintptr_t base;
	
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* screens[2];
	SDL_Texture* overlay;
	SDL_Texture* button;
	SDL_Rect rects[2];
	TTF_Font* font;
	TTF_Font* mini;
	
	int count;
	int current;
	int capacity;
	char** items;
	
	char game_path[MAX_PATH];
	char game_name[MAX_FILE];
	
	int bat;
	int usb;
	
	int synced;
} app;

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
	char cmd[256];
	static const uint8_t volume_raw[21] = {
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
	sprintf(cmd, "vol %i", volume_raw[value]);
	system(cmd);
	settings.volume = value;
}
static void Settings_setBrightness(int value) {
	char cmd[256];
	static const uint8_t brightness_raw[11] = {
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
	sprintf(cmd, "bl %i", brightness_raw[value]);
	system(cmd);
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

static int (*real__libc_start_main)(int (*main)(int,char**,char**), int argc, char **ubp_av, void (*init)(void), void (*fini)(void), void (*rtld_fini)(void), void *stack_end) = NULL;
static int (*real__sprintf_chk)(char *s, int flag, size_t slen, const char *fmt, ...) = NULL;
static int (*real__snprintf_chk)(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...) = NULL;
static void (*real_exit)(int) __attribute__((noreturn)) = NULL;
static void (*real__exit)(int) __attribute__((noreturn)) = NULL;
static int (*real_system)(const char *) = NULL;

// --------------------------------------------
// TODO: tmp? SDL2 compat
// --------------------------------------------

typedef enum SDL_ScaleMode
{
    SDL_ScaleModeNearest, /**< nearest pixel sampling */
    SDL_ScaleModeLinear,  /**< linear filtering */
    SDL_ScaleModeBest     /**< anisotropic filtering */
} SDL_ScaleMode;
int SDL_SetTextureScaleMode(SDL_Texture * texture, SDL_ScaleMode scaleMode);

// --------------------------------------------
// logging
// --------------------------------------------

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
	for (size_t i = 0; i < len; i += 16) {
		printf("%08zx  ", i);
		for (size_t j = 0; j < 16; ++j) {
			if (i + j < len) printf("%02X ", p[i + j]);
			else			 printf("   ");
		}
		printf(" ");
		for (size_t j = 0; j < 16 && i + j < len; ++j) {
			unsigned char c = p[i + j];
			printf("%c", (c >= 32 && c < 127) ? c : '.');
		}
		printf("\n");
	}
	fflush(stdout);
}

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
	}
}
static inline int drastic_is_saving(void) {
	const volatile uint32_t *busy = (const volatile uint32_t *)(app.base + 0x3ec27c);
	return __atomic_load_n(busy, __ATOMIC_ACQUIRE) != 0;
}
static void drastic_await_save(void) {
	while (drastic_is_saving()) SDL_Delay(1);
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

// this is required to handle timing issues with drastic
// I wonder if the timing varies with rom size...yes
#define LOAD_DEFER 35 // was 30
#define RESUME_DEFER 15 // was 10
static struct {
	int loaded;
	int defer;
	int resume;
	int reset;
} preloader = {1,LOAD_DEFER,1,0};

static int preload_game(void) {
	drastic_audio_pause(0);
	preloader.loaded = 0;
	preloader.defer = 0; // only need to defer on boot, not when switching
	preloader.resume = 0;
}
static int preloading_game(void) {
	if (!preloader.loaded && !preloader.defer) {
		preloader.loaded = 1;
		preloader.resume = !preloader.reset;
		preloader.reset = 0;
		preloader.defer = preloader.resume ? RESUME_DEFER : 0;
		drastic_load_nds_and_jump(app.game_path);
		return 1; // never returns because the above jumps
	}
	
	if (preloader.defer) {
		preloader.defer -= 1;
		return 1;
	}
	
	if (preloader.resume) {
		preloader.resume = 0;
		drastic_load_state(0);
	}
	
	return 0;
}

// --------------------------------------------

static inline int getInt(int f) {
    if (f<0) return 0;

    char b[32];
    int n = pread(f, b, sizeof(b) - 1, 0);
    if (n<=0) return 0;

    b[n] = '\0';
    return atoi(b);
}
static inline void putString(const char* path, const char* value) {
	int f = open(path, O_WRONLY);
	if (f<0) return;
	write(f, value, strlen(value));
	close(f);
}

// --------------------------------------------
// custom menu
// --------------------------------------------

static void App_sync(int force) {
	if (force) app.synced = 0;
	
	if (app.synced) return;
	
	if (settings.cropped) {
		if (app.screens[0]) SDL_SetTextureScaleMode(app.screens[0], SDL_ScaleModeNearest);
		if (app.screens[1]) SDL_SetTextureScaleMode(app.screens[1], SDL_ScaleModeNearest);
	}
	else {
		if (app.screens[0]) SDL_SetTextureScaleMode(app.screens[0], SDL_ScaleModeBest);
		if (app.screens[1]) SDL_SetTextureScaleMode(app.screens[1], SDL_ScaleModeBest);
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
static void Device_suspend(void) {
	// real_SDL_RenderClear(app.renderer);
	// real_SDL_RenderPresent(app.renderer);
	
	Settings_save();
	
	drastic_save_state(0);
	drastic_await_save();
	drastic_audio_pause(1);
    
	int fd = open("/sys/power/state", O_WRONLY|O_CLOEXEC);
    if (fd >= 0) {
        write(fd, "mem\n", 4);
        close(fd);
    }
	
	drastic_audio_pause(0);
	Settings_setVolume(settings.volume);
}
static void Device_goodbye(void) {
	SDL_SetRenderDrawColor(app.renderer, BLACK_TRIAD,0xff);
	real_SDL_RenderClear(app.renderer);
	const char* lines[] = {
		"Saving &",
		"shutting",
		"down...",
	};
	SDL_Surface* tmp;
	SDL_Texture* texture;
	int y = (400 - 144) / 2;
	for (int i=0; i<3; i++) {
		tmp = TTF_RenderUTF8_Blended(app.font, lines[i], WHITE_COLOR);
		texture = SDL_CreateTextureFromSurface(app.renderer, tmp);
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
		
		int x = (SCREEN_WIDTH - tmp->w) / 2;
		real_SDL_RenderCopy(app.renderer, texture, NULL, &(SDL_Rect){x,y,tmp->w,tmp->h});
	
		SDL_FreeSurface(tmp);
		SDL_DestroyTexture(texture);
		y += 48; // line_height
	}
	real_SDL_RenderPresent(app.renderer);
}
static void Device_poweroff(void) {
	Device_goodbye();
	Device_goodbye(); // backbuffer too :facepalm:
	
	unlink("/tmp/exec_loop");
	system("vol 0"); // mute
	drastic_save_state(0);
	drastic_quit();
}
static int Device_handleEvent(SDL_Event* event) {
	static int menu_down = 0;
	static int menu_combo = 0;
	static int woken_at = 0;
	static int power_down_at = 0;
	
	if (power_down_at && SDL_GetTicks()-power_down_at>=1000) {
		Device_poweroff();
	}
	
	if (event->type==SDL_KEYDOWN) {
		if (event->key.keysym.scancode==SCAN_POWER && event->key.repeat==0) {
			power_down_at = SDL_GetTicks();
			return 1;
		}
	}
	else if (event->type==SDL_KEYUP) {
		if (event->key.keysym.scancode==SCAN_POWER) {
			power_down_at = 0;
			if (SDL_GetTicks()-woken_at>250) {
				Device_suspend();
				woken_at = SDL_GetTicks();
			}
			return 1;
		}
	}
	else if (event->type==SDL_JOYBUTTONDOWN) {
		if (event->jbutton.button==JOY_MENU) {
			menu_down = 1;
			menu_combo = 0;
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
		
		if (event->jbutton.button==JOY_VOLUP) {
			if (settings.volume<20) {
				Settings_setVolume(settings.volume+1);
			}
			return 1;
		}
		else if (event->jbutton.button==JOY_VOLDN) {
			if (settings.volume>0) {
				Settings_setVolume(settings.volume-1);
			}
			return 1;
		}
		
		if (event->jbutton.button==JOY_R1) {
			if (menu_down) {
				if (settings.brightness<10) {
					Settings_setBrightness(settings.brightness+1);
					menu_combo = 1;
				}
				return 1;
			}
		}
		else if (event->jbutton.button==JOY_L1) {
			if (menu_down) {
				if (settings.brightness>0) {
					Settings_setBrightness(settings.brightness-1);
					menu_combo = 1;
				}
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
		if (len>2 && len<=5) tmp[0] = '\0'; // 1-4 letter extension plus dot (was 1-3, extended for .doom files)
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
    const char* i = *(const char**)a;
    const char* j = *(const char**)b;
    return strcasecmp(i, j);
}

static void App_set(int i) {
	if (i>=app.count) i -= app.count;
	if (i<0) i += app.count;
	app.current = i;
	
	strcpy(settings.game, app.items[app.current]);
	
	static char game_path[MAX_PATH];
	sprintf(game_path, "%s/%s", GAMES_PATH, settings.game);
	strcpy(app.game_path, game_path);
	
	strcpy(app.game_name, settings.game);
	char *dot = strrchr(app.game_name, '.');
	if (dot && dot!=app.game_name) *dot = '\0';
	
	SDL_Log("settings.game: %s", settings.game);
	SDL_Log("app.game_path: %s", app.game_path);
	SDL_Log("app.game_name: %s", app.game_name);
}

static void App_screenshot(int current, int screen) {
	if (!app.screens[screen]) return;
	SDL_Texture* texture = app.screens[screen];
	
	char game_name[MAX_FILE];
	strcpy(game_name, app.items[current]);
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
static void App_preview(int current, int screen) {
	if (!app.screens[screen]) return;
	SDL_Texture* texture = app.screens[screen];
	
	char game_name[MAX_FILE];
	strcpy(game_name, app.items[current]);
	char *dot = strrchr(game_name, '.');
	if (dot && dot!=game_name) *dot = '\0';
	
	char path[MAX_PATH];
	sprintf(path, USERDATA_PATH "/screenshots/%s-%i.bmp", game_name, screen);
	if (access(path, F_OK)!=0) sprintf(path, ASSETS_PATH "/screenshot-%i.png", screen);

	// SDL_Log("preview: %s", path);
	SDL_Surface* tmp = IMG_Load(path);

	int w, h;
	Uint32 format;
	SDL_QueryTexture(texture, &format, NULL, &w, &h);

	if (format==tmp->format->format) {
	    SDL_UpdateTexture(texture, NULL, tmp->pixels, tmp->pitch);
	} else {
	    void *dst;
	    int dst_pitch;
	    if (SDL_LockTexture(texture, NULL, &dst, &dst_pitch) == 0) {
	        SDL_ConvertPixels(w, h, tmp->format->format, tmp->pixels, tmp->pitch, format, dst, dst_pitch);
	        SDL_UnlockTexture(texture);
	    }
	}
	
	SDL_FreeSurface(tmp);
}
static  int App_capture(void) {
	// SDL_Log("capture");
	SDL_Surface *tmp = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_WIDTH, SCREEN_HEIGHT, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_RenderReadPixels(app.renderer, NULL, SDL_PIXELFORMAT_ARGB8888, tmp->pixels, tmp->pitch);
	SDL_SaveBMP(tmp, "/tmp/capture.bmp");
	SDL_FreeSurface(tmp);
}

static void App_init(void) {
	Settings_load();
	
	putString(CPU_PATH "scaling_governor", "userspace");
	putString(CPU_PATH "scaling_setspeed", FREQ_GAME);
	
	// get games
	DIR* dir = opendir(GAMES_PATH);
	if (dir) {
		app.count = 0;
		app.current = 0;
		app.capacity = 16;
		app.items = malloc(sizeof(char*) * app.capacity);
		
		struct dirent* entry;
		while ((entry=readdir(dir))!=NULL) {
			if (entry->d_name[0]=='.') continue;
			if (entry->d_type==DT_DIR) continue;
			
			if (app.count>=app.capacity) {
				app.capacity *= 2;
				app.items = realloc(app.items, sizeof(char*) * app.capacity);
			}
			app.items[app.count++] = strdup(entry->d_name);
		}
		closedir(dir);
		
		qsort(app.items, app.count, sizeof(char*), App_sort);
	}
	
	// get index of last played game
	if (*settings.game) {
		for (int i=0; i<app.count; i++) {
		    if (strcmp(app.items[i], settings.game)==0) {
				app.current = i;
		        break;
		    }
		}
	}
	
	App_set(app.current);
	
	TTF_Init();
	app.font = TTF_OpenFont(ASSETS_PATH "/Inter_24pt-BlackItalic.ttf", 48);
	app.mini = TTF_OpenFont(ASSETS_PATH "/Inter_24pt-Black.ttf", 24);
	
	app.bat = open(BAT_PATH "capacity", O_RDONLY);
	app.usb = open(USB_PATH "online", O_RDONLY);
}
static void App_quit(void) {
	SDL_Log("App_quit");
	
	for (size_t i=0; i<app.count; i++) {
        free(app.items[i]);
    }
    free(app.items);
	
	if (app.overlay) SDL_DestroyTexture(app.overlay);
	if (app.button) SDL_DestroyTexture(app.button);
	
	close(app.bat);
	close(app.usb);
	
	TTF_CloseFont(app.mini);
	TTF_CloseFont(app.font);
	TTF_Quit();
	
	Settings_save();
}
static void App_render(void) {
	if (!app.renderer) return;
	SDL_SetRenderDrawColor(app.renderer, BLACK_TRIAD,0xff);
	real_SDL_RenderClear(app.renderer);
	
	if (!app.screens[0] || !app.screens[1]) return;
	App_sync(0);
	real_SDL_RenderCopy(app.renderer, app.screens[0], NULL, &app.rects[0]);
	real_SDL_RenderCopy(app.renderer, app.screens[1], NULL, &app.rects[1]);
}
static int App_wrap(TTF_Font* font, char* text, SDL_Surface** lines, int max_lines) {
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
		if (strcmp(word, "-") == 0) {
			if (strlen(line) > 0 && line_count < max_lines) {
				lines[line_count++] = TTF_RenderUTF8_Blended(font, line, WHITE_COLOR);
				line[0] = '\0';
			}
			while (isspace(*p)) p++;
			continue;
		}
	
		// append to line
		char test_line[1024] = {0};
		if (strlen(line)>0) sprintf(test_line,"%s %s", line, word);
		else sprintf(test_line,"%s", word);
	
		TTF_SizeUTF8(font, test_line, &line_width, NULL);
		
		int ow = line_count==0 ? 96 : 0;
		if (line_width<SCREEN_WIDTH-ow) {
			strcpy(line, test_line);
		}
		else {
			if (line_count<max_lines) {
				SDL_Surface* txt = TTF_RenderUTF8_Blended(font, line, WHITE_COLOR);
				lines[line_count++] = txt;
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
		SDL_Surface* txt = TTF_RenderUTF8_Blended(font, line, WHITE_COLOR);
		lines[line_count++] = txt;
	}
	return line_count;
}
static int App_button(const char* button, const char* hint, int x, int y) {
	int tw,bw,bh;
	SDL_QueryTexture(app.button, NULL, NULL, &bw, &bh);
	SDL_Surface* tmp;
	SDL_Texture* texture;
	
	if (x<0) {
		x += SCREEN_WIDTH;
		int w;
		TTF_SizeUTF8(app.mini, hint, &w, NULL);
		x -= w;
		x -= 6;
		
		if (strlen(button)==1) {
			x -= bw;
		}
		else {
			TTF_SizeUTF8(app.mini, button, &w, NULL);
			x -= 10 + w + 10;
		}
	}
	

	if (strlen(button)==1) {
		// background
		real_SDL_RenderCopy(app.renderer, app.button, NULL, &(SDL_Rect){x,y,bw,bh});
		y += 2;
		
		// button name
		tmp = TTF_RenderUTF8_Blended(app.mini, button, BLACK_COLOR);
		texture = SDL_CreateTextureFromSurface(app.renderer, tmp);
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
	
		int ox = (bw - tmp->w) / 2;
		real_SDL_RenderCopy(app.renderer, texture, NULL, &(SDL_Rect){x+ox,y,tmp->w,tmp->h});
	
		SDL_FreeSurface(tmp);
		SDL_DestroyTexture(texture);
	}
	else {
		// button name
		tmp = TTF_RenderUTF8_Blended(app.mini, button, BLACK_COLOR);
		texture = SDL_CreateTextureFromSurface(app.renderer, tmp);
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
		
		int fw = 10 + tmp->w + 10;
		
		int w2 = bw/2;
		real_SDL_RenderCopy(app.renderer, app.button, &(SDL_Rect){0,0,w2,bh}, &(SDL_Rect){x,y,w2,bh});
		real_SDL_RenderCopy(app.renderer, app.button, &(SDL_Rect){w2-1,0,2,bh}, &(SDL_Rect){x+w2,y,fw-(w2+w2),bh});
		real_SDL_RenderCopy(app.renderer, app.button, &(SDL_Rect){w2,0,w2,bh}, &(SDL_Rect){x+fw-w2,y,w2,bh});
		y += 2;
		
		int ox = 10;
		real_SDL_RenderCopy(app.renderer, texture, NULL, &(SDL_Rect){x+ox,y,tmp->w,tmp->h});
	
		SDL_FreeSurface(tmp);
		SDL_DestroyTexture(texture);
		
		bw = fw;
	}
	
	tw = bw;
	x += bw;
	
	tw += 6;
	x += 6;
	
	// hint
	tmp = TTF_RenderUTF8_Blended(app.mini, hint, WHITE_COLOR);
	texture = SDL_CreateTextureFromSurface(app.renderer, tmp);
	SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
	
	real_SDL_RenderCopy(app.renderer, texture, NULL, &(SDL_Rect){x,y,tmp->w,tmp->h});
	
	tw += tmp->w;
	
	SDL_FreeSurface(tmp);
	SDL_DestroyTexture(texture);
	
	return tw;
}
static int App_battery(int x, int y, int battery, int is_charging) {
	if (battery<=10) SDL_SetRenderDrawColor(app.renderer, RED_TRIAD,0xff);
	else if (battery<=20) SDL_SetRenderDrawColor(app.renderer, YELLOW_TRIAD,0xff);
	else if (battery>=100) SDL_SetRenderDrawColor(app.renderer, GREEN_TRIAD,0xff);
	else SDL_SetRenderDrawColor(app.renderer, WHITE_TRIAD,0xff);

	x = 414;
	y = 5;

	int w = CEIL_TO(battery,20) * 40 / 100;
	
	const SDL_Rect rects[] = {
		// body
		{x+ 0,y+ 1, 1,30},
		{x+ 1,y+ 0, 3,32},
		{x+ 4,y+ 0,50, 4},
		{x+ 4,y+28,50, 4},
		{x+54,y+ 0, 3,32},
		{x+57,y+ 1, 1,30},
		// cap
		{x+58,y+ 8, 3,16},
		{x+61,y+ 9, 1,14},
		// fill
		{x+  8,y+ 9, 1,14},
		{x+  9,y+ 8, w,16},
		{x+w+9,y+ 9, 1,14},
	};
	SDL_RenderFillRects(app.renderer, rects, NUMBER_OF(rects));

	// antialias
	if (battery<=10) SDL_SetRenderDrawColor(app.renderer, RED_TRIAD,0x80);
	else if (battery<=20) SDL_SetRenderDrawColor(app.renderer, YELLOW_TRIAD,0x80);
	else if (battery>=100) SDL_SetRenderDrawColor(app.renderer, GREEN_TRIAD,0x80);
	else SDL_SetRenderDrawColor(app.renderer, WHITE_TRIAD,0x80);
	
	const SDL_Point points[] = {
		// outer
		{x+ 0,y+ 0},
		{x+57,y+ 0},
		{x+ 0,y+31},
		{x+57,y+31},
		// cap
		{x+61,y+ 8},
		{x+61,y+23},
		// inner
		{x+ 4,y+ 4},
		{x+53,y+ 4},
		{x+ 4,y+27},
		{x+53,y+27},
		// fill
		{x+  8,y+ 8},
		{x+w+9,y+ 8},
		{x+  8,y+23},
		{x+w+9,y+23},
	};
	SDL_RenderDrawPoints(app.renderer, points, NUMBER_OF(points));

	if (is_charging) {
		x -= 24;
		y += 6;
		
		if (battery<=10) SDL_SetRenderDrawColor(app.renderer, RED_TRIAD,0xff);
		else if (battery<=20) SDL_SetRenderDrawColor(app.renderer, YELLOW_TRIAD,0xff);
		else if (battery>=100) SDL_SetRenderDrawColor(app.renderer, GREEN_TRIAD,0xff);
		else SDL_SetRenderDrawColor(app.renderer, WHITE_TRIAD,0xff);
		
		const SDL_Rect rects[] = {
			// top left
			{x+ 6,y+ 0, 6,2},
			{x+ 5,y+ 2, 6,2},
			{x+ 4,y+ 4, 6,2},
			{x+ 3,y+ 6, 6,2},
			// middle
			{x+ 9,y+ 7,10,1},
			{x+ 2,y+ 8,16,2},
			{x+ 1,y+10,16,2},
			{x+ 0,y+12,10,1},
			// bottom right
			{x+10,y+12, 6,2},
			{x+ 9,y+14, 6,2},
			{x+ 8,y+16, 6,2},
			{x+ 7,y+18, 6,2},
			
		};
		SDL_RenderFillRects(app.renderer, rects, NUMBER_OF(rects));
		
		// antialias
		if (battery<=10) SDL_SetRenderDrawColor(app.renderer, RED_TRIAD,0x80);
		else if (battery<=20) SDL_SetRenderDrawColor(app.renderer, YELLOW_TRIAD,0x80);
		else if (battery>=100) SDL_SetRenderDrawColor(app.renderer, GREEN_TRIAD,0x80);
		else SDL_SetRenderDrawColor(app.renderer, WHITE_TRIAD,0x80);
	
		const SDL_Point points[] = {
			// top left
			{x+ 5,y+ 1},
			{x+ 4,y+ 3},
			{x+ 3,y+ 5},
			{x+ 2,y+ 7},
			{x+ 1,y+ 9},
			{x+ 0,y+11},
			// top right
			{x+12,y+ 0},
			{x+11,y+ 2},
			{x+10,y+ 4},
			{x+ 9,y+ 6},
			// bottom left
			{x+ 9,y+13},
			{x+ 8,y+15},
			{x+ 7,y+17},
			{x+ 6,y+19},
			// bottom right
			{x+18,y+ 8},
			{x+17,y+10},
			{x+16,y+12},
			{x+15,y+14},
			{x+14,y+16},
			{x+13,y+18},
		};
		SDL_RenderDrawPoints(app.renderer, points, NUMBER_OF(points));
	}
}
static void App_menu(void) {
	putString(CPU_PATH "scaling_setspeed", FREQ_MENU);
	
	static int last_battery = 0;
	static int was_charging = 0;
	
	int current = app.current;
	App_screenshot(current, 0);
	App_screenshot(current, 1);
	
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
	
	// TODO: remove
	if (!app.button) {
		tmp = TTF_RenderUTF8_Blended(app.mini, BUTTON_BG, WHITE_COLOR);
		app.button = SDL_CreateTextureFromSurface(app.renderer, tmp);
		SDL_SetTextureBlendMode(app.button, SDL_BLENDMODE_BLEND);
		SDL_FreeSurface(tmp);
	}
	
	int selected = 0;
	int capture = 0;
	int dirty = 1;
	int in_menu = 1;
	SDL_Event event;
	while (in_menu) {
		while (in_menu && real_SDL_PollEvent(&event)) {
			LOG_event(&event);
			
			// TODO: shouldn't this be gated by event.type==SDL_JOYBUTTONDOWN?
			if (event.jbutton.button==JOY_L2 || event.jbutton.button==JOY_R2) dirty = 1;
			
			if (Device_handleEvent(&event)) continue;
			
			if (event.type==SDL_JOYBUTTONDOWN) {
				if (event.jbutton.button==JOY_UP) {
					selected -= 1;
					dirty = 1;
				}
				else if (event.jbutton.button==JOY_DOWN) {
					selected += 1;
					dirty = 1;
				}
				
				if (event.jbutton.button==JOY_RIGHT) {
					selected = 0;
					current += 1;
					if (current>=app.count) current -= app.count;
					dirty = 1;
				}
				else if (event.jbutton.button==JOY_LEFT) {
					selected = 0;
					current -= 1;
					if (current<0) current += app.count;
					dirty = 1;
				}
				
				if (event.jbutton.button==JOY_B) { // BACK
					if (current!=app.current) {
						current = app.current;
						selected = 0;
						dirty = 1;
					}
					else {
						in_menu = 0;
					}
				}
				else if (event.jbutton.button==JOY_A) { // SELECT
					if (current==app.current) {
						if (selected==3) { // BACK
							// buh
						}
						else if (selected==0) { // SAVE
							drastic_save_state(0);
						}
						else if (selected==1) { // LOAD
							drastic_load_state(0);
						}
						else if (selected==2) { // RESET
							preloader.reset = 1;
							preload_game();
						}
						in_menu = 0;
					}
					else {
						if (selected==1) { // BACK
							current = app.current;
							selected = 0;
							dirty = 1;
						}
						else if (selected==0) { // LOAD
							drastic_save_state(0);
							App_set(current);
							Settings_save();
							preload_game();
							
							in_menu = 0;
						}
					}
				}
				
				if (event.jbutton.button==JOY_START) { // TODO: tmp
					drastic_save_state(0);
					drastic_quit();
					in_menu = 0;
				}
				
				// TODO: tmp
				if (event.jbutton.button==JOY_L1) { // capture
					capture = 1;
					dirty = 1;
				}
			}
			else if (event.type==SDL_JOYBUTTONUP) {
				if (event.jbutton.button==JOY_MENU) {
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
			App_preview(current,0);
			App_preview(current,1);
			
			// screens and gradient
			App_render();
			
			real_SDL_RenderCopy(app.renderer, app.overlay, NULL, NULL);
			
			int x,y,w,h;
			
			// battery
			App_battery(414,5, battery,is_charging);
			
			// game name
			char name[MAX_FILE];
			App_getDisplayName(app.items[current], name);
	
			#define MAX_LINES 8
			SDL_Surface* lines[MAX_LINES] = {0};
			int line_count = App_wrap(app.font, name, lines, MAX_LINES);
			
			int line_height = 48;
			h = line_count * line_height;
			x = 0;
			y = -10;
			for (int i=0; i<line_count; i++) {
				SDL_Surface* line = lines[i];
				SDL_Texture* texture = SDL_CreateTextureFromSurface(app.renderer, line);
				SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
				real_SDL_RenderCopy(app.renderer, texture, NULL, &(SDL_Rect){x,y,line->w,line->h});
				SDL_FreeSurface(line);
				SDL_DestroyTexture(texture);
				lines[i] = NULL;
				y += line_height;
			}
			
			// relative to game name (shifts)
			// y += 12;
			// h = (SCREEN_HEIGHT / 2) - y;
			
			// center in bottom "screen"
			y = h = SCREEN_HEIGHT / 2;
			
			// center in full screen
			// y = 0;
			// h = SCREEN_HEIGHT;

			char* save_items[] = {
				"SAVE",
				"LOAD",
				"RESET",
			};
			char* load_items[] = {
				"LOAD",
			};
			
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
			
			if (selected<0) selected += count;
			selected %= count;
			
			TTF_SizeUTF8(app.mini, "RESET", &w, NULL);
			w = 8 + w + 8;
			x = (SCREEN_WIDTH - w) / 2;
			
			int oh = (((count-1) * 40) + 32);
			y += (h - oh) / 2;
			h = oh;
			
			x -= 8;
			y -= 8;
			w = 8 + w + 8;
			h = 8 + h + 8;
			SDL_SetRenderDrawColor(app.renderer, BLACK_TRIAD,0x60);
			const SDL_Rect rects[] = {
				{x+  0,y+ 1,  1,h-2},
				{x+  1,y+ 0,w-2,h  },
				{x+w-1,y+ 1,  1,h-2},
			};
			SDL_RenderFillRects(app.renderer, rects, NUMBER_OF(rects));
			
			w = -8 + w -8;
			h = -8 + h -8;
			x += 8;
			y += 8;
			
			for (int i=0; i<count; i++) {
				SDL_Color color = WHITE_COLOR;
				if (i==selected) {
					color = BLACK_COLOR;
					SDL_SetRenderDrawColor(app.renderer, WHITE_TRIAD,0xff);
					
					const SDL_Rect rects[] = {
						{x+  0,y+ 1,  1,30},
						{x+  1,y+ 0,w-2,32},
						{x+w-1,y+ 1,  1,30},
					};
					SDL_RenderFillRects(app.renderer, rects, NUMBER_OF(rects));
			
					// corners
					SDL_SetRenderDrawColor(app.renderer, WHITE_TRIAD,0x80);
					const SDL_Point points[] = {
						{x+  0,y+ 0},
						{x+w-1,y+ 0},
						{x+  0,y+31},
						{x+w-1,y+31},
					};
					SDL_RenderDrawPoints(app.renderer, points, NUMBER_OF(points));
				}
				
				tmp = TTF_RenderUTF8_Blended(app.mini, items[i], color);
				SDL_Texture* texture = SDL_CreateTextureFromSurface(app.renderer, tmp);
				SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
				real_SDL_RenderCopy(app.renderer, texture, NULL, &(SDL_Rect){x+8,y+1,tmp->w,tmp->h});
				SDL_FreeSurface(tmp);
				SDL_DestroyTexture(texture);
				
				y += 40;
			}
			
			// flip
			real_SDL_RenderPresent(app.renderer);
			
			if (capture) {
				App_capture();
				capture = 0;
			}
		}
		else {
			SDL_Delay(16);
		}
	}
	
	putString(CPU_PATH "scaling_setspeed", FREQ_GAME);
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
		int result = real_SDL_PollEvent(event);
		if (!result) return 0;
		
		if (Device_handleEvent(event)) continue;
		
		if (event->type==SDL_JOYBUTTONDOWN) {
			if (event->jbutton.button==JOY_MENU) {
				continue;
			}
		}
		else if (event->type==SDL_JOYBUTTONUP) {
			if (event->jbutton.button==JOY_MENU) {
				App_menu();
				continue;
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

				// clamp to bottom screen
				if (x>=SCREEN_WIDTH) x = SCREEN_WIDTH-1;
				else if (x<0) x = 0;
				if (y>=src.y+src.h) y = src.y+src.h-1;
				else if (y<src.y) y = src.y;

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
	return 1; // complete render takeover
}

SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, Uint32 flags) {
	app.renderer = real_SDL_CreateRenderer(window, index, flags);
	return app.renderer;
}
int SDL_RenderClear(SDL_Renderer* renderer) {
	return 1; // complete render takeover
}
void SDL_RenderPresent(SDL_Renderer * renderer) {
	if (preloading_game()) return;
	
	App_render(); // complete render takeover
	real_SDL_RenderPresent(renderer);
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int type, int w, int h) {
	SDL_Texture *texture = real_SDL_CreateTexture(renderer, format, type, w, h);
	if (type==SDL_TEXTUREACCESS_STREAMING && w==256 && h==192) {
		if (!app.screens[0]) app.screens[0] = texture;
		else if (!app.screens[1]) app.screens[1] = texture;
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

// --------------------------------------------
// hijack main to modify args
// --------------------------------------------

#define DISABLE_LOGGING

#ifdef DISABLE_LOGGING
int __printf_chk(int flag, const char *fmt, ...) {
	return 0;
}
int puts (const char *__s) {
	return 0;
}
#endif

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
	
	return drastic_main(2, new_argv, envp);
}

int __libc_start_main(int (*main)(int,char**,char**), int argc, char **ubp_av, void (*init)(void), void (*fini)(void), void (*rtld_fini)(void), void *stack_end) {
	hook();
	drastic_main = main;
	return real__libc_start_main(override_args, argc, ubp_av, init, fini, rtld_fini, stack_end);
}