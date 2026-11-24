/*
 * FRANK DOS — the keyboard BIOS: INT 09h's native half and INT 16h.
 *
 * The ROM's INT 09h reads port 60h and offers the byte to INT 15h/4Fh; what
 * is left comes here as AL: the shift states in the BDA, the translation to
 * a BIOS key word (DnCraptor's bios_09h.c tables, SeaBIOS's rules), the
 * keyboard buffer. INT 16h reads the buffer; with nothing in it a read
 * waits in the ROM stub (STI, HLT) instead of here.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include "hle/hle.h"

#define K_FLAGS1  BDA(0x17)
#define K_FLAGS2  BDA(0x18)
#define K_HEAD    BDA(0x1A)
#define K_TAIL    BDA(0x1C)
#define K_START   BDA(0x80)
#define K_END     BDA(0x82)
#define K_FLAG3   BDA(0x96)          /* E0/E1 seen, right Ctrl/Alt */

enum { F_RSHIFT = 0x01, F_LSHIFT = 0x02, F_CTRL = 0x04, F_ALT = 0x08,
       F_SCROLL = 0x10, F_NUM = 0x20, F_CAPS = 0x40, F_INS = 0x80 };
enum { F2_LCTRL = 0x01, F2_LALT = 0x02, F2_SCROLL = 0x10, F2_NUM = 0x20, F2_CAPS = 0x40 };
enum { F3_E1 = 0x01, F3_E0 = 0x02, F3_RCTRL = 0x04, F3_RALT = 0x08 };

/* ---- the buffer --------------------------------------------------------- */
static bool kbuf_put(uint16_t key)
{
	uint16_t tail = g_rd16(K_TAIL), next = tail + 2;
	if (next >= g_rd16(K_END))
		next = g_rd16(K_START);
	if (next == g_rd16(K_HEAD))
		return false;                       /* full */
	g_wr16(0x400 + tail, key);
	g_wr16(K_TAIL, next);
	return true;
}

static bool kbuf_peek(uint16_t *key)
{
	uint16_t head = g_rd16(K_HEAD);
	if (head == g_rd16(K_TAIL))
		return false;
	*key = g_rd16(0x400 + head);
	return true;
}

static void kbuf_drop(void)
{
	uint16_t head = g_rd16(K_HEAD) + 2;
	if (head >= g_rd16(K_END))
		head = g_rd16(K_START);
	g_wr16(K_HEAD, head);
}

/* ---- translation (bios_09h.c) ---------------------------------------------- */
static const char normal[] = {
	0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
	'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/'};
static const char shifted[] = {
	0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0,
	'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
	'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?'};
/* the keypad with NumLock (or Shift): 47h-53h */
static const char keypad[] = "789-456+1230.";

static uint8_t to_ascii(uint8_t scan, bool shift, bool ctrl, bool caps, bool num)
{
	if (scan >= 0x47 && scan <= 0x53 && scan != 0x4A && scan != 0x4E)
		return (num != shift) ? (uint8_t)keypad[scan - 0x47] : 0;
	switch (scan) {
	case 0x37: return '*';
	case 0x4A: return '-';
	case 0x4E: return '+';
	case 0x01: return 0x1B;
	case 0x0E: return ctrl ? 0x7F : 0x08;
	case 0x0F: return shift ? 0 : 0x09;
	case 0x1C: return ctrl ? 0x0A : 0x0D;
	case 0x39: return ' ';
	}
	if (scan >= sizeof normal)
		return 0;
	if (ctrl) {
		char b = normal[scan];
		if (b >= 'a' && b <= 'z')
			return (uint8_t)(b - 'a' + 1);
		switch (b) {
		case '[': return 0x1B;
		case '\\': return 0x1C;
		case ']': return 0x1D;
		case '-': return 0x1F;
		}
		return 0;
	}
	char c = shift ? shifted[scan] : normal[scan];
	if (caps && c >= 'a' && c <= 'z')
		c = (char)(c - 'a' + 'A');
	else if (caps && c >= 'A' && c <= 'Z')
		c = (char)(c - 'A' + 'a');
	return (uint8_t)c;
}

static uint16_t key_word(uint8_t scan, uint8_t ascii, uint8_t fl)
{
	bool shift = fl & (F_LSHIFT | F_RSHIFT), ctrl = fl & F_CTRL, alt = fl & F_ALT;
	if (alt) {
		if ((ascii >= 'a' && ascii <= 'z') || (ascii >= 'A' && ascii <= 'Z'))
			return (uint16_t)scan << 8;
		if (scan >= 0x02 && scan <= 0x0D)       /* Alt+1..= */
			return (uint16_t)(scan + 0x76) << 8;
	}
	if (scan >= 0x3B && scan <= 0x44) {         /* F1-F10 */
		if (alt) return (uint16_t)(0x68 + scan - 0x3B) << 8;
		if (ctrl) return (uint16_t)(0x5E + scan - 0x3B) << 8;
		if (shift) return (uint16_t)(0x54 + scan - 0x3B) << 8;
	}
	if (scan == 0x57 || scan == 0x58) {         /* F11, F12 */
		int n = scan - 0x57;
		return (uint16_t)((alt ? 0x8B : ctrl ? 0x89 : shift ? 0x87 : 0x85) + n) << 8;
	}
	if (ctrl && !ascii) {                       /* Ctrl with the cursor keys */
		switch (scan) {
		case 0x47: return 0x7700;
		case 0x4B: return 0x7300;
		case 0x4D: return 0x7400;
		case 0x4F: return 0x7500;
		case 0x49: return 0x8400;
		case 0x51: return 0x7600;
		}
	}
	return (uint16_t)scan << 8 | ascii;
}

/* INT 09h, past INT 15h/4Fh: AL the byte; AH 1 to have the ROM call INT 1Bh */
enum hle_result hle_int09(CPUI386 *cpu)
{
	uint8_t raw = AL;
	AH = 0;
	uint8_t f3 = g_rd8(K_FLAG3);
	if (raw == 0xE0 || raw == 0xE1) {
		g_wr8(K_FLAG3, (f3 & ~(F3_E0 | F3_E1)) | (raw == 0xE0 ? F3_E0 : F3_E1));
		return HLE_DONE;
	}
	bool e0 = f3 & F3_E0, e1 = f3 & F3_E1;
	g_wr8(K_FLAG3, f3 & ~(F3_E0 | F3_E1));
	uint8_t scan = raw & 0x7F;
	bool up = raw & 0x80;
	uint8_t fl = g_rd8(K_FLAGS1), fl2 = g_rd8(K_FLAGS2);
	f3 &= ~(F3_E0 | F3_E1);

	switch (scan) {
	case 0x2A:                              /* shifts (E0 2A: a fake one) */
	case 0x36:
		if (e0)
			return HLE_DONE;
		{
			uint8_t b = scan == 0x2A ? F_LSHIFT : F_RSHIFT;
			g_wr8(K_FLAGS1, up ? (fl & ~b) : (fl | b));
		}
		return HLE_DONE;
	case 0x1D:                              /* Ctrl */
		if (e1)
			return HLE_DONE;                /* Pause's prefix */
		if (e0)
			f3 = up ? (f3 & ~F3_RCTRL) : (f3 | F3_RCTRL);
		else
			fl2 = up ? (fl2 & ~F2_LCTRL) : (fl2 | F2_LCTRL);
		fl = ((fl2 & F2_LCTRL) || (f3 & F3_RCTRL)) ? (fl | F_CTRL) : (fl & ~F_CTRL);
		g_wr8(K_FLAGS1, fl); g_wr8(K_FLAGS2, fl2); g_wr8(K_FLAG3, f3);
		return HLE_DONE;
	case 0x38:                              /* Alt */
		if (e0)
			f3 = up ? (f3 & ~F3_RALT) : (f3 | F3_RALT);
		else
			fl2 = up ? (fl2 & ~F2_LALT) : (fl2 | F2_LALT);
		fl = ((fl2 & F2_LALT) || (f3 & F3_RALT)) ? (fl | F_ALT) : (fl & ~F_ALT);
		g_wr8(K_FLAGS1, fl); g_wr8(K_FLAGS2, fl2); g_wr8(K_FLAG3, f3);
		return HLE_DONE;
	case 0x3A:                              /* Caps Lock */
		if (!up && !(fl2 & F2_CAPS))
			fl ^= F_CAPS;
		fl2 = up ? (fl2 & ~F2_CAPS) : (fl2 | F2_CAPS);
		g_wr8(K_FLAGS1, fl); g_wr8(K_FLAGS2, fl2);
		return HLE_DONE;
	case 0x45:                              /* Num Lock (E1 1D 45: Pause) */
		if (e1)
			return HLE_DONE;
		if (!up && !(fl2 & F2_NUM))
			fl ^= F_NUM;
		fl2 = up ? (fl2 & ~F2_NUM) : (fl2 | F2_NUM);
		g_wr8(K_FLAGS1, fl); g_wr8(K_FLAGS2, fl2);
		return HLE_DONE;
	case 0x46:                              /* Scroll Lock; E0 46: Ctrl+Break */
		if (e0) {
			if (!up) {
				g_wr16(K_HEAD, g_rd16(K_START));
				g_wr16(K_TAIL, g_rd16(K_START));
				kbuf_put(0x0000);
				g_wr8(BDA(0x71), 0x80);
				AH = 1;
			}
			return HLE_DONE;
		}
		if (!up && !(fl2 & F2_SCROLL))
			fl ^= F_SCROLL;
		fl2 = up ? (fl2 & ~F2_SCROLL) : (fl2 | F2_SCROLL);
		g_wr8(K_FLAGS1, fl); g_wr8(K_FLAGS2, fl2);
		return HLE_DONE;
	case 0x52:
		if (!up && !e0 && !(fl & (F_NUM | F_LSHIFT | F_RSHIFT)))
			g_wr8(K_FLAGS1, fl ^ F_INS);
		else if (!up && e0)
			g_wr8(K_FLAGS1, fl ^ F_INS);
		break;
	case 0x53:                              /* Ctrl+Alt+Del */
		if (!up && (fl & F_CTRL) && (fl & F_ALT)) {
			g_wr16(BDA(0x72), 0x1234);
			outb(0x64, 0xFE);
			return HLE_DONE;
		}
		break;
	}
	if (up || scan == 0)
		return HLE_DONE;

	bool shift = fl & (F_LSHIFT | F_RSHIFT);
	uint8_t ascii = e0 && scan >= 0x47 && scan <= 0x53 ? 0 :
		to_ascii(scan, shift, fl & F_CTRL, fl & F_CAPS, fl & F_NUM);
	uint16_t key = key_word(scan, ascii, fl);
	if (e0) {
		if (scan == 0x1C)
			key = 0xE00D;
		else if (scan == 0x35)
			key = 0xE02F;
		else if (!(fl & (F_ALT | F_CTRL)))
			key = (key & 0xFF00) | 0xE0;
	}
	if (key)
		kbuf_put(key);
	return HLE_DONE;
}

/* the old services (00h, 01h) see no extended-only keys, and 00h for E0 */
static bool key_ok_old(uint16_t *k)
{
	uint8_t scan = *k >> 8, ascii = *k & 0xFF;
	if (*k == 0xE00D) { *k = 0x1C0D; return true; }
	if (*k == 0xE02F) { *k = 0x352F; return true; }
	if (scan >= 0x85)
		return false;
	if (ascii == 0xE0 && scan)
		*k &= 0xFF00;
	return true;
}

enum hle_result hle_int16(CPUI386 *cpu)
{
	uint16_t k;
	switch (AH) {
	case 0x00:
	case 0x10:
		for (;;) {
			if (!kbuf_peek(&k))
				return HLE_WAIT;
			kbuf_drop();
			if (AH == 0x10 || key_ok_old(&k))
				break;
		}
		AX = k;
		return HLE_DONE;
	case 0x01:
	case 0x11:
		for (;;) {
			if (!kbuf_peek(&k)) {
				hle_zf(cpu, true);
				return HLE_DONE;
			}
			if (AH == 0x11 || key_ok_old(&k))
				break;
			kbuf_drop();
		}
		AX = k;
		hle_zf(cpu, false);
		return HLE_DONE;
	case 0x02:
		AL = g_rd8(K_FLAGS1);
		return HLE_DONE;
	case 0x12: {
		uint8_t f2 = g_rd8(K_FLAGS2), f3 = g_rd8(K_FLAG3);
		AL = g_rd8(K_FLAGS1);
		AH = (f2 & 0x73) | (f3 & F3_RCTRL ? 0x04 : 0) | (f3 & F3_RALT ? 0x08 : 0);
		return HLE_DONE;
	}
	case 0x03:
		return HLE_DONE;
	case 0x05:
		AL = kbuf_put(CX) ? 0 : 1;
		return HLE_DONE;
	case 0x09:
		AL = 0x20;                          /* AH=10h-12h supported */
		return HLE_DONE;
	case 0x0A:
		BX = 0x41AB;                        /* MF2 */
		return HLE_DONE;
	default:
		return HLE_DONE;
	}
}

void kbd_post(void)
{
	g_wr8(K_FLAGS1, F_NUM);
	g_wr8(K_FLAGS2, 0);
	/* the 8042: IRQ 1, set 1 translation, the keyboard on */
	outb(0x64, 0x60);
	outb(0x60, 0x45);
	outb(0x64, 0xAE);
}
