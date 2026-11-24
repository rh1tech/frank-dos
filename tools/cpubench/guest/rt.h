/* cpubench guest runtime: freestanding, entered at 0x400000 with ESP set,
 * leaves its result in EAX and halts. */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef int s32;

u32 bench_main(void);

__attribute__((section(".text.start"), naked, used)) void _start(void)
{
	__asm__ volatile("call bench_main\n\thlt\n\tjmp _start");
}

void *memcpy(void *d, const void *s, unsigned n)
{
	void *r = d;
	__asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
	return r;
}

void *memset(void *d, int c, unsigned n)
{
	void *r = d;
	__asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
	return r;
}

int memcmp(const void *a, const void *b, unsigned n)
{
	const u8 *p = a, *q = b;
	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p - *q;
	return 0;
}

static u32 seed = 12345;
static inline u32 rnd(void)
{
	seed = seed * 1103515245u + 12345u;
	return seed >> 8;
}
