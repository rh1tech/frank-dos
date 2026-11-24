/* memcpy/memset (rep movs/stos), strlen/strcmp loops, a 4bpp blit with masks */
#include "rt.h"

static u8 a[65536], bb[65536];
static char s1[512], s2[512];
static u8 fb[400 * 300];          /* 4bpp 800x600 */
static u8 glyph[16][2];

static unsigned my_strlen(const char *s) { const char *p = s; while (*p) p++; return p - s; }
static int my_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) a++, b++;
	return (u8)*a - (u8)*b;
}

static void movsd(void *d, const void *s, unsigned n)
{
	__asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}

u32 bench_main(void)
{
	u32 sum = 0;
	for (unsigned i = 0; i < sizeof a; i++) a[i] = rnd();
	for (int r = 0; r < 20; r++) {
		memcpy(bb + (r & 7), a + 3, 30000);
		movsd(bb, a + 4 * r, 8000);
		memset(a + 100 * r, r, 5000);
		sum += bb[r * 37] + a[r * 1000];
	}
	for (int i = 0; i < 511; i++) { s1[i] = 'a' + i % 23; s2[i] = s1[i]; }
	for (int r = 0; r < 200; r++) {
		s2[500 - r] = 'z';
		sum += my_strlen(s1 + r) + my_strcmp(s1, s2);
		s2[500 - r] = s1[500 - r];
	}
	for (int i = 0; i < 16; i++) { glyph[i][0] = rnd(); glyph[i][1] = rnd(); }
	/* draw 8x16 glyphs at nibble positions: read-modify-write with masks */
	for (int r = 0; r < 3000; r++) {
		int x = (r * 13) % 784, y = (r * 7) % 284;
		u8 col = r & 15;
		for (int gy = 0; gy < 16; gy++) {
			u8 *row = fb + (y + gy) * 400 + x / 2;
			u32 bits = glyph[gy][0] << 8 | glyph[gy][1];
			for (int gx = 0; gx < 8; gx++) {
				if (!(bits & (0x8000 >> gx))) continue;
				int px = x + gx;
				u8 *p = row + (px - x + (x & 1)) / 2;
				if (px & 1) *p = (*p & 0xf0) | col;
				else *p = (*p & 0x0f) | col << 4;
			}
		}
	}
	for (unsigned i = 0; i < sizeof fb; i += 64) sum += fb[i];
	return sum;
}
