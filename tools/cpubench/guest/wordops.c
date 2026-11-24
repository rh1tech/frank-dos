/* 16- and 8-bit data: movzx/movsx, 66-prefixed arithmetic, bitfields, a struct walk */
#include "rt.h"

struct node { struct node *next; u16 a, b; u8 flags, kind; short delta; };
static struct node nodes[4096];
static u16 w[8192];

u32 bench_main(void)
{
	u32 sum = 0;
	for (int i = 0; i < 4096; i++) {
		nodes[i].next = &nodes[(i * 1597 + 1) & 4095];
		nodes[i].a = rnd(); nodes[i].b = rnd();
		nodes[i].flags = rnd(); nodes[i].kind = i & 7; nodes[i].delta = rnd();
	}
	struct node *n = &nodes[0];
	for (int r = 0; r < 60000; r++) {
		switch (n->kind) {
		case 0: n->a += n->b; break;
		case 1: n->b ^= n->a >> 3; break;
		case 2: n->delta = -n->delta; break;
		case 3: n->flags |= 0x10; break;
		case 4: n->flags &= ~0x10; n->a--; break;
		default: sum += (u16)(n->a * n->b) + n->delta; break;
		}
		sum += n->flags & 0x10 ? n->a : n->b;
		n = n->next;
	}
	for (int i = 0; i < 8192; i++) w[i] = i * 7;
	for (int r = 0; r < 10; r++)
		for (int i = 1; i < 8192; i++) {
			u16 v = w[i] + w[i - 1];
			w[i] = (u16)(v >> 1) | (u16)(v << 15);
			sum += (short)w[i] < 0;
		}
	return sum;
}
