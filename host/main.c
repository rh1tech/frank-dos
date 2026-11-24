/*
 * FRANK DOS on the host: the firmware's machine (CPU core, devices, native
 * BIOS, native FreeDOS kernel) in an SDL window, for debugging DOS without
 * the board.
 *
 *   frankdos-host [-sd DIR] [-hda IMG] [-hdb IMG] [-fda IMG] [-fdb IMG] [-sdboot]
 *                 [-type TEXT] [-typeat SECONDS] [-shot FILE.ppm SECONDS] [-headless]
 *
 * DIR stands in for the SD card (default ./sd); the images are under DIR/386,
 * as on a card set up for frank-386. stdin takes the firmware's console
 * commands without the ~C ...~ wrapping: "type TEXT" (\r is Enter, {N} a raw
 * keycode), "screen", "regs", "shot FILE.ppm", "quit".
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include <SDL.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pc.h"
#include "i8042.h"
#include "pcspk.h"
#include "adlib.h"
#include "sb16.h"
#include "sn76489.h"
#include "dss.h"
#include "autotype.h"
#include "hle/hle.h"
#include "hle/boot.h"
#include "fdos/dosrt.h"
#include "pico/time.h"

extern const char *host_sd_root;           /* ff_host.c */

PC *pc;
uint8_t gfx_buffer[256ul << 10];            /* the VGA's RAM (pc.c) */
volatile uint32_t dos_diag_kernel_code;

#define FB_W 720
#define FB_H 480
static uint32_t fb[FB_W * FB_H];
static volatile int fb_dirty;
static PCConfig config;

/* ---- what the firmware's main.c and dos_api.c give the rest ------------------- */
uint64_t cpu_port_time_us(void) { return time_us_64(); }
uint32_t get_uticks(void) { return time_us_32(); }
void *pcmalloc(long size) { return malloc(size); }

int load_rom(void *phys_mem, const char *file, uword addr, int backward)
{
	(void)phys_mem; (void)file; (void)addr; (void)backward;
	return -1;                              /* (no SeaBIOS: the BIOS is native) */
}


/* ---- video ----------------------------------------------------------------- */
static void redraw(void *opaque, int x, int y, int w, int h)
{
	(void)opaque; (void)x; (void)y; (void)w; (void)h;
	fb_dirty = 1;
}

static void poll(void *opaque) { (void)opaque; }

static int save_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
	for (int i = 0; i < FB_W * FB_H; i++) {
		uint32_t c = fb[i];
		uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	return 0;
}

/* ---- sound: the firmware's mixer (drivers/audio/audio.c), pulled by SDL ----- */
int16_t midi_sample(void);

static void mix_one(int16_t *l, int16_t *r)
{
	static uint32_t dss_phase;
	static int dss_v;
	if (pc->dss_enabled) {
		dss_phase += 7000;
		if (dss_phase >= 44100) {
			dss_phase -= 44100;
			dss_v = dss_sample();
		}
	}
	int b_v = 0, r_v = 0, l_v = 0;
	if (pc->pcspk_enabled)
		b_v = pcspk_sample(pc->pcspk);
	if (pc->covox_enabled && pc->covox_sample) {
		int16_t s = ((int16_t)pc->covox_sample - 127) << 8;
		r_v += s;
		l_v += s;
	}
	if (pc->tandy_enabled) {
		int16_t s = sn76489_sample();
		r_v += s;
		l_v += s;
	}
	if (pc->adlib_enabled) {
		int16_t s = adlib_getsample(pc->adlib);
		r_v += s;
		l_v += s;
	}
	if (pc->sb16_enabled)
		sb16_getsample(pc->sb16, &r_v, &l_v);
	r_v += dss_v;
	l_v += dss_v;
	if (b_v)
		r_v = l_v = 16384;
	if (r_v > 32767) r_v = 32767;
	if (r_v < -32768) r_v = -32768;
	if (l_v > 32767) l_v = 32767;
	if (l_v < -32768) l_v = -32768;
	*l = (int16_t)l_v;
	*r = (int16_t)r_v;
}

static void audio_cb(void *user, Uint8 *stream, int len)
{
	(void)user;
	int16_t *s = (int16_t *)stream;
	for (int i = 0; i < len / 4; i++)
		mix_one(&s[2 * i], &s[2 * i + 1]);
}

/* ---- keyboard: SDL scancodes to the Linux key codes ps2_put_keycode takes --- */
static int sdl_to_linux(SDL_Scancode sc)
{
	switch (sc) {
	case SDL_SCANCODE_ESCAPE: return 1;
	case SDL_SCANCODE_1: return 2; case SDL_SCANCODE_2: return 3; case SDL_SCANCODE_3: return 4;
	case SDL_SCANCODE_4: return 5; case SDL_SCANCODE_5: return 6; case SDL_SCANCODE_6: return 7;
	case SDL_SCANCODE_7: return 8; case SDL_SCANCODE_8: return 9; case SDL_SCANCODE_9: return 10;
	case SDL_SCANCODE_0: return 11; case SDL_SCANCODE_MINUS: return 12; case SDL_SCANCODE_EQUALS: return 13;
	case SDL_SCANCODE_BACKSPACE: return 14; case SDL_SCANCODE_TAB: return 15;
	case SDL_SCANCODE_Q: return 16; case SDL_SCANCODE_W: return 17; case SDL_SCANCODE_E: return 18;
	case SDL_SCANCODE_R: return 19; case SDL_SCANCODE_T: return 20; case SDL_SCANCODE_Y: return 21;
	case SDL_SCANCODE_U: return 22; case SDL_SCANCODE_I: return 23; case SDL_SCANCODE_O: return 24;
	case SDL_SCANCODE_P: return 25; case SDL_SCANCODE_LEFTBRACKET: return 26; case SDL_SCANCODE_RIGHTBRACKET: return 27;
	case SDL_SCANCODE_RETURN: return 28; case SDL_SCANCODE_LCTRL: return 29;
	case SDL_SCANCODE_A: return 30; case SDL_SCANCODE_S: return 31; case SDL_SCANCODE_D: return 32;
	case SDL_SCANCODE_F: return 33; case SDL_SCANCODE_G: return 34; case SDL_SCANCODE_H: return 35;
	case SDL_SCANCODE_J: return 36; case SDL_SCANCODE_K: return 37; case SDL_SCANCODE_L: return 38;
	case SDL_SCANCODE_SEMICOLON: return 39; case SDL_SCANCODE_APOSTROPHE: return 40; case SDL_SCANCODE_GRAVE: return 41;
	case SDL_SCANCODE_LSHIFT: return 42; case SDL_SCANCODE_BACKSLASH: return 43;
	case SDL_SCANCODE_Z: return 44; case SDL_SCANCODE_X: return 45; case SDL_SCANCODE_C: return 46;
	case SDL_SCANCODE_V: return 47; case SDL_SCANCODE_B: return 48; case SDL_SCANCODE_N: return 49;
	case SDL_SCANCODE_M: return 50; case SDL_SCANCODE_COMMA: return 51; case SDL_SCANCODE_PERIOD: return 52;
	case SDL_SCANCODE_SLASH: return 53; case SDL_SCANCODE_RSHIFT: return 54; case SDL_SCANCODE_KP_MULTIPLY: return 55;
	case SDL_SCANCODE_LALT: return 56; case SDL_SCANCODE_SPACE: return 57; case SDL_SCANCODE_CAPSLOCK: return 58;
	case SDL_SCANCODE_F1: return 59; case SDL_SCANCODE_F2: return 60; case SDL_SCANCODE_F3: return 61;
	case SDL_SCANCODE_F4: return 62; case SDL_SCANCODE_F5: return 63; case SDL_SCANCODE_F6: return 64;
	case SDL_SCANCODE_F7: return 65; case SDL_SCANCODE_F8: return 66; case SDL_SCANCODE_F9: return 67;
	case SDL_SCANCODE_F10: return 68; case SDL_SCANCODE_NUMLOCKCLEAR: return 69; case SDL_SCANCODE_SCROLLLOCK: return 70;
	case SDL_SCANCODE_KP_7: return 71; case SDL_SCANCODE_KP_8: return 72; case SDL_SCANCODE_KP_9: return 73;
	case SDL_SCANCODE_KP_MINUS: return 74; case SDL_SCANCODE_KP_4: return 75; case SDL_SCANCODE_KP_5: return 76;
	case SDL_SCANCODE_KP_6: return 77; case SDL_SCANCODE_KP_PLUS: return 78; case SDL_SCANCODE_KP_1: return 79;
	case SDL_SCANCODE_KP_2: return 80; case SDL_SCANCODE_KP_3: return 81; case SDL_SCANCODE_KP_0: return 82;
	case SDL_SCANCODE_KP_PERIOD: return 83; case SDL_SCANCODE_F11: return 87; case SDL_SCANCODE_F12: return 88;
	case SDL_SCANCODE_KP_ENTER: return 96; case SDL_SCANCODE_RCTRL: return 97; case SDL_SCANCODE_KP_DIVIDE: return 98;
	case SDL_SCANCODE_PRINTSCREEN: return 99; case SDL_SCANCODE_RALT: return 100;
	case SDL_SCANCODE_HOME: return 102; case SDL_SCANCODE_UP: return 103; case SDL_SCANCODE_PAGEUP: return 104;
	case SDL_SCANCODE_LEFT: return 105; case SDL_SCANCODE_RIGHT: return 106; case SDL_SCANCODE_END: return 107;
	case SDL_SCANCODE_DOWN: return 108; case SDL_SCANCODE_PAGEDOWN: return 109; case SDL_SCANCODE_INSERT: return 110;
	case SDL_SCANCODE_DELETE: return 111; case SDL_SCANCODE_PAUSE: return 119;
	default: return 0;
	}
}

/* ---- the console, on stdin ---------------------------------------------------- */
static int quit;

/* "\r" and "\n" as the keys (the board's bench tools send the characters) */
static void type_text(const char *t)
{
	static char buf[1024];
	size_t k = 0;
	for (; *t && k < sizeof buf - 1; t++) {
		if (t[0] == '\\' && (t[1] == 'r' || t[1] == 'n')) {
			buf[k++] = '\r';
			t++;
		} else {
			buf[k++] = *t;
		}
	}
	buf[k] = 0;
	autotype_text(buf);
}

static void console_line(char *line)
{
	size_t n = strlen(line);
	while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		line[--n] = 0;
	if (!strncmp(line, "type ", 5)) {
		type_text(line + 5);
	} else if (!strncmp(line, "mouse ", 6)) {      /* "mouse DX DY BUTTONS" */
		int dx = 0, dy = 0, b = 0;
		sscanf(line + 6, "%d %d %d", &dx, &dy, &b);
		ps2_mouse_event(pc->mouse, dx, dy, 0, b);
	} else if (!strncmp(line, "dbg21 ", 6)) {      /* the next N INT 21h calls, on stdout */
		extern int fdos_dbg21_left;
		fdos_dbg21_left = atoi(line + 6);
	} else if (!strcmp(line, "cds")) {
		extern void fdos_dump_cds(void);
		fdos_dump_cds();
	} else if (!strcmp(line, "screen")) {
		hle_screen();
	} else if (!strcmp(line, "regs")) {
		hle_report();
	} else if (!strncmp(line, "shot ", 5)) {
		printf(save_ppm(line + 5) ? "shot: cannot write %s\n" : "shot: %s\n", line + 5);
	} else if (!strcmp(line, "quit")) {
		quit = 1;
	} else if (n) {
		printf("commands: type TEXT, mouse DX DY BUTTONS, screen, regs, shot FILE.ppm, quit\n");
	}
	fflush(stdout);
}

static void console_poll(void)
{
	static char buf[1024];
	static size_t len;
	char c;
	while (read(0, &c, 1) == 1) {
		if (c == '\n' || len == sizeof buf - 1) {
			buf[len] = 0;
			console_line(buf);
			len = 0;
		} else {
			buf[len++] = c;
		}
	}
}

int main(int argc, char **argv)
{
	const char *img[4] = { NULL, NULL, NULL, NULL };    /* fda fdb hda hdb */
	const char *type = NULL, *shot = NULL;
	double shot_s = 0, type_s = 0;
	int headless = 0, sdboot = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-sd") && i + 1 < argc) host_sd_root = argv[++i];
		else if (!strcmp(argv[i], "-fda") && i + 1 < argc) img[0] = argv[++i];
		else if (!strcmp(argv[i], "-fdb") && i + 1 < argc) img[1] = argv[++i];
		else if (!strcmp(argv[i], "-hda") && i + 1 < argc) img[2] = argv[++i];
		else if (!strcmp(argv[i], "-hdb") && i + 1 < argc) img[3] = argv[++i];
		else if (!strcmp(argv[i], "-type") && i + 1 < argc) type = argv[++i];
		else if (!strcmp(argv[i], "-typeat") && i + 1 < argc) type_s = atof(argv[++i]);
		else if (!strcmp(argv[i], "-shot") && i + 2 < argc) { shot = argv[++i]; shot_s = atof(argv[++i]); }
		else if (!strcmp(argv[i], "-headless")) headless = 1;
		else if (!strcmp(argv[i], "-sdboot")) sdboot = 1;
		else {
			fprintf(stderr, "usage: %s [-sd DIR] [-hda IMG] [-hdb IMG] [-fda IMG] [-fdb IMG]"
				" [-type TEXT] [-typeat SECONDS] [-shot FILE.ppm SECONDS] [-headless]\n", argv[0]);
			return 2;
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);

	SDL_Window *win = NULL;
	SDL_Renderer *ren = NULL;
	SDL_Texture *tex = NULL;
	if (SDL_Init(headless ? SDL_INIT_AUDIO : SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return 1;
	}
	if (!headless) {
		win = SDL_CreateWindow("FRANK DOS (host)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
				       FB_W * 2, FB_H * 2, SDL_WINDOW_RESIZABLE);
		ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
		SDL_RenderSetLogicalSize(ren, FB_W, FB_H);
		tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);
	}

	/* the firmware's load_default_config(), and what init_emulator() does next */
	config.mem_size = (8l << 20) - FDOS_ARENA_BYTES;
	config.vga_mem_size = EMU_VGA_MEM_SIZE_KB * 1024;
	config.cpu_gen = 4;
	config.fpu = 1;
	config.width = FB_W;
	config.height = FB_H;
	config.redirector = 1;
	/* -sdboot: the firmware's boot=sd, the SD directory is C: */
	extern bool hle_boot_sd;
	extern void netredirect_set_letter(char letter);
	hle_boot_sd = sdboot;
	if (sdboot)
		netredirect_set_letter('C');
	int13_set_images("386", img[0], img[1], sdboot ? NULL : img[2], sdboot ? NULL : img[3]);

	pc = pc_new(redraw, poll, NULL, (u8 *)fb, &config);
	if (!pc) {
		fprintf(stderr, "pc_new failed\n");
		return 1;
	}
	dosrt_psram_init();
	pc->paused = 0;
	pc->pcspk_enabled = 1;
	pc->adlib_enabled = 1;
	pc->sb16_enabled = 1;
	pc->tandy_enabled = 0;
	pc->covox_enabled = 0;
	pc->mpu401_enabled = 0;
	pc->dss_enabled = 0;
	pc->mouse_enabled = 1;

	SDL_AudioSpec want = { 0 }, have;
	want.freq = SOUND_FREQUENCY;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 512;
	want.callback = audio_cb;
	SDL_AudioDeviceID audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (audio)
		SDL_PauseAudioDevice(audio, 0);

	hle_boot(pc);

	uint64_t t0 = time_us_64(), last_present = 0;
	while (!quit) {
		autotype_tick();
		if (hle_cpu_running) {
			for (int i = 0; i < 10; i++) {
				if (!(dosrt_active() && pc->cpu->native_done))
					pc_step(pc);
				dosrt_after_batch();
			}
		} else {
			pc_vga_step(pc);
			sleep_us(500);
		}
		host_timers_run();
		if (pc->reset_request) {
			pc->reset_request = 0;
			hle_boot(pc);
		}
		if (pc->shutdown_state)
			break;

		uint64_t now = time_us_64();
		if (now - last_present < 16000)
			continue;
		last_present = now;
		console_poll();
		if (type && now - t0 >= (uint64_t)(type_s * 1e6)) {
			type_text(type);
			type = NULL;
		}
		if (shot && now - t0 >= (uint64_t)(shot_s * 1e6)) {
			save_ppm(shot);
			printf("shot: %s\n", shot);
			shot = NULL;
			if (headless)
				break;
		}
		if (headless)
			continue;
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT)
				quit = 1;
			else if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
				uint32_t st = SDL_GetMouseState(NULL, NULL);
				int b = (st & SDL_BUTTON_LMASK ? 1 : 0) | (st & SDL_BUTTON_RMASK ? 2 : 0) | (st & SDL_BUTTON_MMASK ? 4 : 0);
				int dx = e.type == SDL_MOUSEMOTION ? e.motion.xrel : 0;
				int dy = e.type == SDL_MOUSEMOTION ? e.motion.yrel : 0;
				ps2_mouse_event(pc->mouse, dx, dy, 0, b);      /* (screen Y: it flips it) */
			}
			else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
				int k = sdl_to_linux(e.key.keysym.scancode);
				if (k)
					ps2_put_keycode(pc->kbd, e.type == SDL_KEYDOWN, k);
			}
		}
		if (fb_dirty) {
			fb_dirty = 0;
			SDL_UpdateTexture(tex, NULL, fb, FB_W * 4);
		}
		SDL_RenderClear(ren);
		SDL_RenderCopy(ren, tex, NULL, NULL);
		SDL_RenderPresent(ren);
	}
	if (audio)
		SDL_CloseAudioDevice(audio);
	SDL_Quit();
	return 0;
}
