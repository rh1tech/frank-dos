/* CRC32, insertion and quick sort, a hash table, recursion */
#include "rt.h"

static u32 crctab[256];
static u8 buf[16384];
static s32 arr[2048];
struct ent { u32 key; u16 val; u8 used, pad; } tab[1024];

static u32 crc32(const u8 *p, unsigned n)
{
	u32 c = ~0u;
	while (n--)
		c = crctab[(c ^ *p++) & 0xff] ^ (c >> 8);
	return ~c;
}

static void qsort_i(s32 *a, int lo, int hi)
{
	while (lo < hi) {
		s32 p = a[(lo + hi) / 2];
		int i = lo, j = hi;
		while (i <= j) {
			while (a[i] < p) i++;
			while (a[j] > p) j--;
			if (i <= j) { s32 t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
		}
		if (j - lo < hi - i) { qsort_i(a, lo, j); lo = i; }
		else { qsort_i(a, i, hi); hi = j; }
	}
}

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

u32 bench_main(void)
{
	u32 sum = 0;
	for (u32 i = 0; i < 256; i++) {
		u32 c = i;
		for (int k = 0; k < 8; k++)
			c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crctab[i] = c;
	}
	for (unsigned i = 0; i < sizeof buf; i++)
		buf[i] = rnd();
	for (int r = 0; r < 4; r++)
		sum += crc32(buf, sizeof buf);
	for (int r = 0; r < 6; r++) {
		for (int i = 0; i < 2048; i++)
			arr[i] = rnd() - 0x400000;
		qsort_i(arr, 0, 2047);
		for (int i = 1; i < 2048; i++)
			sum += arr[i] < arr[i - 1];
		sum += arr[1000];
	}
	for (int r = 0; r < 20000; r++) {
		u32 k = rnd() & 0xffff;
		u32 h = (k * 2654435761u) >> 22;
		while (tab[h].used && tab[h].key != k)
			h = (h + 1) & 1023;
		if (tab[h].used) { tab[h].val++; sum += tab[h].val; }
		else if (r & 1) { tab[h].used = 1; tab[h].key = k; tab[h].val = 1; }
		if ((r & 255) == 0) for (int i = 0; i < 1024; i++) tab[i].used = 0;
	}
	sum += fib(20);
	return sum;
}
