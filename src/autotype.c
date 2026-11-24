/**
 * frank-386 — scripted keystroke injection. See autotype.h.
 */
#include <stdlib.h>
#include "autotype.h"

#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pc.h"
#include "i8042.h"

extern PC *pc;

/* Linux input keycodes, as ps2_put_keycode() expects. */
#define K_ESC 1
#define K_ENTER 28
#define K_SPACE 57
#define K_BACKSLASH 43
#define K_DOT 52
#define K_MINUS 12
#define K_UP    103
#define K_DOWN  108
#define K_LEFT  105
#define K_RIGHT 106
#define K_SEMICOLON 39
#define K_SLASH 53
#define K_LSHIFT 42
#define SHIFTED 0x100   /* ascii_to_key: the key with Shift held */

static const uint8_t k_digit[10] = { 11, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

/* a..z in keycode order */
static const uint8_t k_alpha[26] = {
    30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
    49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44
};

static int ascii_to_key(char c) {
    if (c >= 'a' && c <= 'z') return k_alpha[c - 'a'];
    if (c >= 'A' && c <= 'Z') return k_alpha[c - 'A'];
    if (c >= '0' && c <= '9') return k_digit[c - '0'];
    switch (c) {
    case '\r': case '\n': return K_ENTER;
    case ' ':  return K_SPACE;
    case '\\': return K_BACKSLASH;
    case '.':  return K_DOT;
    case '-':  return K_MINUS;
    case 0x1b: return K_ESC;
    /* Arrow keys, for driving in-game menus. Deliberately punctuation no
     * DOS command line needs - not letters, which would shadow real
     * input ('v' for Down would break typing "vc"). */
    case '^': return K_UP;
    case '!': return K_DOWN;
    case '<': return K_LEFT;
    case '>': return K_RIGHT;
    case ':': return SHIFTED | K_SEMICOLON;     /* "h:" for the SD drive */
    case '/': return K_SLASH;
    case '*': return SHIFTED | 9;               /* Shift+8: wildcards */
    default:   return -1;   /* skip rather than mistype */
    }
}

/*
 * Timing. The guest needs to reach a command prompt before anything is
 * typed, and DOS drops keys pressed faster than it polls, so keys go in
 * slowly and with a real down/up pair each.
 */
#define AT_START_DELAY_US  15000000u   /* let BIOS, DOS and the shell settle */
#define AT_KEY_PERIOD_US      90000u   /* ~11 keys/sec */

/* What is still to type: the build's AUTOTYPE_STR once DOS has settled,
 * then whatever the console's "~C type TEXT~" queues (autotype_text). */
static char queue[512];
static uint32_t q_head, q_tail;          /* free-running; index & (size - 1) */

/* "{N}" is the raw keycode N, "{M+N}" N with M held (frank-dos: F-keys,
 * Ctrl/Alt combinations); in the queue as 0x01, M (0: none), N */
void autotype_text(const char *text) {
    for (; *text; text++) {
        if (q_tail - q_head + 3 > sizeof(queue)) return;   /* full: dropped */
        if (*text == '{') {
            char *end;
            long a = strtol(text + 1, &end, 10), m = 0;
            if (*end == '+') {
                m = a;
                a = strtol(end + 1, &end, 10);
            }
            if (*end == '}' && a > 0 && a < 256 && m >= 0 && m < 256) {
                queue[q_tail++ & (sizeof(queue) - 1)] = 0x01;
                queue[q_tail++ & (sizeof(queue) - 1)] = (char)m;
                queue[q_tail++ & (sizeof(queue) - 1)] = (char)a;
                text = end;
                continue;
            }
        }
        queue[q_tail++ & (sizeof(queue) - 1)] = *text;
    }
}

void autotype_tick(void) {
    static uint64_t next_us;
    static int phase;                    /* 0 shift, 1 down, 2 up, 3 unshift */
    static int key;
    static int mod;                      /* the key held around it (0: none) */

    if (!pc || !pc->kbd) return;

    const uint64_t now = time_us_64();
#ifdef AUTOTYPE_STR
    static bool seeded;
    if (!seeded) {
        if (next_us == 0) { next_us = now + AT_START_DELAY_US; return; }
        if (now < next_us) return;
        autotype_text(AUTOTYPE_STR);
        seeded = true;
    }
#endif
    if (now < next_us || q_head == q_tail) return;

    const char c = queue[q_head & (sizeof(queue) - 1)];
    /* '~' is a pause, not a keystroke. Loading a game takes seconds and
     * the keys that follow it would otherwise all be typed into a
     * program that is not listening yet. */
    if (phase == 0 && c == '~') {
        q_head++;
        next_us = now + 3000000u;
        return;
    }
    if (phase == 0) {
        if (c == 0x01) {                 /* a raw key: 0x01, modifier, key */
            if (q_tail - q_head < 3) return;
            mod = (uint8_t)queue[(q_head + 1) & (sizeof(queue) - 1)];
            key = (uint8_t)queue[(q_head + 2) & (sizeof(queue) - 1)];
            q_head += 2;
        } else {
            key = ascii_to_key(c);
            if (key < 0) { q_head++; return; }
            mod = (key & SHIFTED) ? K_LSHIFT : 0;
        }
        phase = 1;
        if (mod) {
            ps2_put_keycode(pc->kbd, 1, mod);
            next_us = now + 20000u;
            return;
        }
    }
    if (phase == 1) {
        ps2_put_keycode(pc->kbd, 1, key & 0xff);
        phase = 2;
        next_us = now + 20000u;          /* hold briefly */
    } else if (phase == 2) {
        ps2_put_keycode(pc->kbd, 0, key & 0xff);
        phase = mod ? 3 : 0;
        if (!phase) { q_head++; next_us = now + AT_KEY_PERIOD_US; }
        else next_us = now + 20000u;
    } else {
        ps2_put_keycode(pc->kbd, 0, mod);
        phase = 0;
        q_head++;
        next_us = now + AT_KEY_PERIOD_US;
    }
}

