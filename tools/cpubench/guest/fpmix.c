/* x87: double loops, int<->double, a small matrix multiply in int too */
#include "rt.h"

static double m1[24][24], m2[24][24], m3[24][24];
static s32 i1[32][32], i2[32][32], i3[32][32];

u32 bench_main(void)
{
	u32 sum = 0;
	for (int i = 0; i < 24; i++)
		for (int j = 0; j < 24; j++) { m1[i][j] = (i * 3 + j) * 0.25; m2[i][j] = (i - j) / 7.0; }
	for (int r = 0; r < 3; r++)
		for (int i = 0; i < 24; i++)
			for (int j = 0; j < 24; j++) {
				double s = 0;
				for (int k = 0; k < 24; k++) s += m1[i][k] * m2[k][j];
				m3[i][j] = s;
			}
	sum += (s32)(m3[5][7] * 1000);
	double x = 1.0;
	for (int i = 0; i < 20000; i++) { x = x * 1.0001 + 0.5 / (i + 1); if (x > 1e6) x *= 0.001; }
	sum += (s32)x;
	for (int i = 0; i < 32; i++)
		for (int j = 0; j < 32; j++) { i1[i][j] = rnd() & 0xfff; i2[i][j] = rnd() & 0xfff; }
	for (int r = 0; r < 4; r++)
		for (int i = 0; i < 32; i++)
			for (int j = 0; j < 32; j++) {
				s32 s = 0;
				for (int k = 0; k < 32; k++) s += i1[i][k] * i2[k][j];
				i3[i][j] = s;
			}
	sum += i3[3][9];
	return sum;
}
