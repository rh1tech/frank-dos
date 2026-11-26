/*
 * FRANK DOS — the FreeDOS kernel's runtime: coroutines instead of nesting.
 *
 * murm386 runs its native kernel nested in the CPU loop: native code that
 * needs guest code (a BIOS INT, a device driver, a child program) steps the
 * CPU itself, "while (!done) pc_step(pc, n)", on the one native stack. Here
 * core 0's stack is 4 KB of SCRATCH_Y, and the CPU loop must stay on it.
 *
 * So every entry into the kernel runs on a coroutine whose stack is in the
 * native arena at the top of PSRAM (FDOS_NATIVE_ARENA): INT 20h-2Fh from the
 * guest, and the boot (INT 19h, which never returns: the kernel then runs
 * the shell). pc_step() in the kernel yields: the main loop runs a CPU batch
 * and resumes the innermost coroutine, which looks at its condition again,
 * exactly as its loop did. cpu->native_done ("stop the guest") makes the main
 * loop resume it without running the guest at all. DOS nests last in, first
 * out, so the coroutines' stacks are a stack: each one below the SP of the
 * one it interrupted.
 *
 * On every switch into the kernel the core's lazy flags are folded
 * (cpu_getflags), so cpu->flags is exact and the port's cf/zf bitfields
 * (fdos_port.h) may be read and written directly.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include "fdos_port.h"
#include "fdos.h"
#include "hle/coro.h"
#include "fdos/dosrt.h"
#include "hle/rom_layout.h"

#define CPU_CS_OF(c) ((uint16_t)(c)->seg[SEG_CS].sel)
#define LIN_SS_SP(c) (((uint32_t)(c)->seg[SEG_SS].sel << 4) + (uint16_t)(c)->gprx[regsp].r16)

#define MAX_DEPTH 24
#define RED_ZONE  128              /* below an interrupted coroutine's SP */

typedef struct {
	coro_t *co;
	handler_t fn;               /* the INT handler (NULL: the boot) */
	uint8_t dl;                 /* the boot drive */
	uint16_t stub_cs, stub_ip;  /* where the guest entered */
	uint16_t entry_ss, entry_sp;    /* and its stack then (its interrupt frame) */
	bool finished, result;
	uint8_t *arena_sp;          /* where it left the arena for an application's stack */
	uint16_t app_ss, app_sp;    /* and the guest's stack then */
} dctx_t;

int fdos_dbg21_left;
int fdos_dbg_intcall;           /* (~C dbg21 N~: also the next N INT 21h calls from the kernel/apps) */
static CPU *g_cpu;
static bios_callback_params_t *cb_head;
static coro_t *main_co;
static dctx_t ctx[MAX_DEPTH];
static int depth;
static uint8_t *arena_lo, *arena_hi;
static handler_t handlers[256];

static void fold_flags(void) { (void)cpu_getflags(g_cpu); }

static void ctx_main(void *arg)
{
	dctx_t *c = arg;
	CPU *cpu = g_cpu;
	fold_flags();
	if (c->fn)
		c->result = c->fn(cpu);
	else {
		CPU_DL = c->dl;
		_boot(cpu);
		kernel(cpu);                /* (never returns: the shell runs on) */
	}
	c->finished = true;
	coro_switch(c->co, main_co);
	for (;;)
		;
}

/* the stack of a new coroutine: below whatever the innermost one uses */
static bool ctx_push(handler_t fn, uint8_t dl)
{
	if (depth >= MAX_DEPTH) {
		printf("dosrt: kernel entries nested %d deep\n", depth);
		return false;
	}
	uint8_t *hi = arena_hi;
	if (depth) {
		uint8_t *sp = (uint8_t *)coro_sp(ctx[depth - 1].co);
		/* a native application runs on its own stack (in guest RAM): the
		 * coroutine that runs it has the arena down to where it left it */
		if ((sp < arena_lo || sp >= arena_hi) && ctx[depth - 1].arena_sp)
			sp = ctx[depth - 1].arena_sp;
		if (sp < hi)
			hi = sp - RED_ZONE;
	}
	if (hi - arena_lo < 4096) {
		printf("dosrt: the native arena is used up (%d deep)\n", depth);
		return false;
	}
	dctx_t *c = &ctx[depth];
	memset(c, 0, sizeof *c);
	c->fn = fn;
	c->dl = dl;
	c->stub_cs = CPU_CS_OF(g_cpu);
	c->stub_ip = (uint16_t)g_cpu->ip;
	c->entry_ss = (uint16_t)g_cpu->seg[SEG_SS].sel;
	c->entry_sp = g_cpu->gprx[regsp].r16;
	c->co = coro_new(ctx_main, c, arena_lo, (size_t)(hi - arena_lo));
	if (!c->co)
		return false;
	/* no MSPLIM: a native application (EZ/ELF) runs on a stack of its own,
	 * which the kernel allocates in guest memory, below this arena */
	coro_nolimit(c->co);
	depth++;
	return true;
}

/* switch into the innermost coroutine; back here when it yields or ends */
static void run_top(void)
{
	dctx_t *c = &ctx[depth - 1];
	fold_flags();
	coro_switch(main_co, c->co);
	if (!c->finished)
		return;
	CPU *cpu = g_cpu;
	/* a handler that returned true IRETs: the stub's IRET, if the guest is
	 * still at its trap (anything else moved CS:IP on purpose) */
	if (c->fn && c->result && CPU_CS == c->stub_cs && CPU_IP == c->stub_ip) {
		cpu->ip = (uint16_t)(c->stub_ip + 4);
		cpu->next_ip = cpu->ip;
		PREFETCH_RESET
	}
	if (fdos_dbg21_left > 0 && c->fn == handlers[0x21] && (CPU_AH != 0x2C || (CPU_AX & 0xFF) != 0))
		printf("  -> ax %04x frame cf %d\n", CPU_AX, g_rd16(LIN_SS_SP(cpu) + 4) & 1);
	coro_free(c->co);
	depth--;
}

extern char __fdos_data_start__[], __fdos_data_end__[], __fdos_data_source__[];
extern char __fdos_bss_start__[], __fdos_bss_end__[];

void dosrt_psram_init(void)
{
#ifdef FRANK_HOST
	return;             /* (the host keeps the kernel's data where it was linked) */
#endif
	memcpy(__fdos_data_start__, __fdos_data_source__, (size_t)(__fdos_data_end__ - __fdos_data_start__));
	memset(__fdos_bss_start__, 0, (size_t)(__fdos_bss_end__ - __fdos_bss_start__));
	printf("dosrt: kernel data %u + %u bytes in PSRAM at %p\n",
	       (unsigned)(__fdos_data_end__ - __fdos_data_start__),
	       (unsigned)(__fdos_bss_end__ - __fdos_bss_start__), (void *)__fdos_data_start__);
}

/* a native application is about to run on its own stack, from the innermost
 * coroutine: its arena SP is about here (this frame is below the caller's) */
void *dosrt_native_stack_enter(void)
{
	volatile uint8_t here;
	if (!depth)
		return NULL;
	dctx_t *c = &ctx[depth - 1];
	CPU *cpu = g_cpu;
	void *old = c->arena_sp;
	c->arena_sp = (uint8_t *)&here;
	c->app_ss = (uint16_t)CPU_SS;       /* (an application inside an application:
	                                     * its own coroutine, its own frame) */
	c->app_sp = (uint16_t)CPU_SP;
	return old;
}

void dosrt_native_stack_leave(void *old)
{
	if (depth)
		ctx[depth - 1].arena_sp = old;
}

/* ---- the main loop's side ------------------------------------------------------ */
void dosrt_init(CPU *cpu)
{
	g_cpu = cpu;
	if (!main_co)
		main_co = coro_self_main();
	/* the top of the 8 MB PSRAM, whatever the guest's RAM below (the JIT's
	 * store may sit between them, jit16.h) */
	arena_hi = hle_ram + (8u << 20);
	arena_lo = arena_hi - (FDOS_ARENA_BYTES - FDOS_DATA_BYTES);
	depth = 0;
	cpu->native_done = false;
}

bool dosrt_active(void) { return depth > 0; }

/* the console's ~C regs~: the kernel's coroutines */
void dosrt_report(void)
{
	printf("dosrt: depth %d native_done %d callbacks:", depth, g_cpu ? g_cpu->native_done : -1);
	for (bios_callback_params_t *q = cb_head; q; q = q->chain)
		printf(" %04x:%04x%s(%s)", q->expected_cs, q->expected_ip, q->done ? "*" : "", q->owner ? q->owner : "");
	printf("\n");
	for (int i = 0; i < depth; i++) {
		uint32_t *sp = coro_sp(ctx[i].co);
		printf("  [%d] %s at %04x:%04x sp %p code:", i, ctx[i].fn ? "int" : "boot", ctx[i].stub_cs, ctx[i].stub_ip,
		       (void *)sp);
		int n = 0;
		for (int k = 0; k < 400 && n < 14 && (uint8_t *)(sp + k) < arena_hi; k++)
			if ((sp[k] & 0xFFF00001u) == 0x10000001u && sp[k] < 0x10200000u) {
				printf(" %08lx", (unsigned long)sp[k]);
				n++;
			}
		printf("\n");
	}
}

void dosrt_after_batch(void)
{
	if (depth)
		run_top();
}

/* a trap at a DOS entry stub (rom.asm, F000:D000 + 8n): INT n */
bool dosrt_int(CPU *cpu, uint8_t n)
{
	handler_t h = handlers[n];
	if (!h) {                           /* (no handler: as an IRET) */
		cpu->next_ip = (uint16_t)(cpu->ip + 4);
		return true;
	}
	cpu->next_ip = cpu->ip;             /* CPU_IP: the stub, as murm386's core */
	if (fdos_dbg21_left > 0 && n == 0x21 && CPU_AH != 0x2C && CPU_AH != 0x0B) {   /* (not VC's polls) */
		fdos_dbg21_left--;
		printf("int21 ax %04x bx %04x cx %04x dx %04x ds %04x es %04x\n", CPU_AX, CPU_BX, CPU_CX, CPU_DX, CPU_DS, CPU_ES);
		if (CPU_AH == 0x40 && CPU_BX <= 2) {    /* (what goes to the screen: the text, up to 80) */
			const uint8_t *t = (const uint8_t *)PSRAM_BASE_ADDR + ((uint32_t)CPU_DS << 4) + CPU_DX;
			printf("  \"");
			for (unsigned i = 0; i < CPU_CX && i < 80; i++)
				printf(t[i] >= 32 && t[i] < 127 ? "%c" : "\\x%02x", t[i]);
			printf("\"\n");
		}
	}
	if (!ctx_push(h, 0)) {
		cpu->next_ip = (uint16_t)(cpu->ip + 4);
		return true;
	}
	run_top();
	return !cpu->native_done;
}

/* INT 19h: the kernel boots, from drive dl (its BIOS number) */
bool dosrt_boot(CPU *cpu, uint8_t dl)
{
	dosrt_init(cpu);
	/* interrupts on, as murm386's INT 19h leaves them: the kernel's waits
	 * (the F5/F8 prompt, the keyboard) count timer ticks */
	cpu_setflags(cpu, IF, 0);
	if (!ctx_push(NULL, dl))
		return false;
	run_top();
	return true;
}

/* ---- the kernel's side ---------------------------------------------------------------- */
void fdos_pc_step(void)
{
	if (!depth) {
		printf("dosrt: pc_step outside the kernel\n");
		return;
	}
	dctx_t *c = &ctx[depth - 1];
	CPU *cpu = g_cpu;
	/* the guest still at the trap that entered the kernel, on the same
	 * frame, would enter it again: it idles in the ROM (STI, HLT) until the
	 * kernel goes on. (Another INT through the same stub is another
	 * frame: that one runs.) While a native application runs from this
	 * coroutine, its EXEC has moved SS:SP to the child's stack: the guest
	 * on that frame idles too (the application's own INTs push one) */
	bool parked = CPU_CS == c->stub_cs && CPU_IP == c->stub_ip &&
		      ((CPU_SS == c->entry_ss && CPU_SP == c->entry_sp) ||
		       (c->arena_sp && CPU_SS == c->app_ss && CPU_SP == c->app_sp));
	bool old_if = parked && ifl;
	if (parked) {
		SET_CS(0xF000);
		SET_IP(ROM_IDLE);
	}
	coro_switch(c->co, main_co);
	/* (resumed: run_top folded the flags) */
	if (parked && CPU_CS == 0xF000 && CPU_IP >= ROM_IDLE && CPU_IP < ROM_IDLE + 4) {
		SET_CS(c->stub_cs);
		SET_IP(c->stub_ip);
		ifl = old_if;
	}
}

void pc_service(struct PC *p) { (void)p; fdos_pc_step(); }

/* callbacks: guest code reaching an address (the sentinel FFEF:000F, the XMS
 * entry F000:FEFF: both FFEFFh) runs its callback */
bool set_bios_callback(CPU *cpu, bios_callback_params_t *p, bool reenter)
{
	(void)cpu;
	(void)reenter;
	for (bios_callback_params_t *q = cb_head; q; q = q->chain)
		if (q == p)
			return true;
	p->chain = cb_head;
	cb_head = p;
	return true;
}

bool drop_bios_callback(CPU *cpu, bios_callback_params_t *p)
{
	(void)cpu;
	for (bios_callback_params_t **q = &cb_head; *q; q = &(*q)->chain)
		if (*q == p) {
			*q = p->chain;
			p->chain = NULL;
			return true;
		}
	return false;
}

/* the trap at FFEFFh: the innermost callback registered for this CS:IP */
bool dosrt_callback(CPU *cpu)
{
	uint16_t cs = CPU_CS, ip = (uint16_t)cpu->ip;
	bios_callback_params_t *p = cb_head;
	while (p && !(p->expected_cs == cs && p->expected_ip == ip))
		p = p->chain;
	cpu->next_ip = cpu->ip;
	if (!p) {
		printf("dosrt: no callback for %04x:%04x\n", cs, ip);
		cpu->native_done = true;
		return false;
	}
	fold_flags();
	if (p->callback(cpu, p)) {          /* an IRET, here */
		uint32_t sp = LIN_SS_SP(cpu);
		uint16_t nip = g_rd16(sp), ncs = g_rd16(sp + 2), nfl = g_rd16(sp + 4);
		CPU_SP += 6;
		SET_CS(ncs);
		SET_IP(nip);
		cpu->flags = (cpu->flags & ~0xFFFFu) | nfl | 2;
	}
	return !cpu->native_done;
}

/* ---- guest calls from the kernel --------------------------------------------------------- */
void cpu_intcall(CPU *cpu, uint8_t intnum)
{
	uint16_t fl = (uint16_t)cpu_getflags(cpu);
	CPU_SP -= 2; g_wr16(LIN_SS_SP(cpu), fl);
	CPU_SP -= 2; g_wr16(LIN_SS_SP(cpu), CPU_CS);
	CPU_SP -= 2; g_wr16(LIN_SS_SP(cpu), CPU_IP);
	ifl = 0;
	tf = 0;
	SET_CS(g_rd16(intnum * 4 + 2));
	SET_IP(g_rd16(intnum * 4));
}

static bool intcall_waiter(CPU *cpu, bios_callback_params_t *params)
{
	if (!params->done) {
		ifl = 0;
		params->done = true;
		cpu->native_done = true;
	} else
		ifl = 1;
	return false;
}

bool terminate_requested(void);

/* INT n in the guest, from the kernel, and back (bios_intcall.c's) */
void bios_intcall(CPU *cpu, uint8_t intnum, const char *owner)
{
	/* INT 2Fh: the redirector hooked at the CPU's INT (netredirect.c, the H:
	 * drive) sees the kernel's calls too, as it sees an INT instruction's */
	if (intnum == 0x2F && cpu->int2f_handler && cpu->int2f_handler(cpu, cpu->int2f_opaque)) {
		fold_flags();
		return;
	}
	uint16_t cs = CPU_CS, ip = CPU_IP;
	bios_callback_params_t params = {
		.callback = intcall_waiter, .expected_cs = 0xFFEF, .expected_ip = 0x000F,
		.done = false, .owner = owner,
	};
	set_bios_callback(cpu, &params, true);
	SET_CS(params.expected_cs);
	SET_IP(params.expected_ip);
	bool old_ifl = ifl;
	bool old_native_done = cpu->native_done;
	cpu_intcall(cpu, intnum);
	cpu->native_done = false;
	unsigned spins = 0;
	while (!params.done) {
		if (terminate_requested())
			break;
		fdos_pc_step();
		spins++;
	}
	{
		extern int fdos_dbg_intcall;
		if (fdos_dbg_intcall > 0 && intnum == 0x21) {
			fdos_dbg_intcall--;
			printf("intcall 21 spins %u done %d ax %04x cs:ip %04x:%04x\n", spins, params.done, CPU_AX, CPU_CS, CPU_IP);
		}
	}
	cpu->native_done = old_native_done;
	drop_bios_callback(cpu, &params);
	ifl = old_ifl;
	SET_CS(cs);
	SET_IP(ip);
}

/* ---- registers, vectors ------------------------------------------------------------------- */
void cpu_save_regs(const CPU *cpu, CPU_regs *r)
{
	for (int i = 0; i < 8; i++)
		r->gprx[i].r32 = cpu->gprx[i].r32;
	r->flags.value = (uint32_t)cpu->flags;
	r->es = CPU_ES;
	r->ds = CPU_DS;
	r->fs = CPU_FS;
	r->gs = CPU_GS;
}

void cpu_restore_regs(CPU *cpu, const CPU_regs *r)
{
	for (int i = 0; i < 8; i++)
		cpu->gprx[i].r32 = r->gprx[i].r32;
	uint32_t f = r->flags.value & 0xFFFFu;    /* (real mode: never VM, RF and the like) */
	cpu_setflags(cpu, f, ~f & 0xFFFFu);
	SET_ES(r->es);
	SET_DS(r->ds);
	SET_FS(r->fs);
	SET_GS(r->gs);
}

static void set_dos_vec(uint8_t n, handler_t h)
{
	handlers[n] = h;
	g_wr16(n * 4, NATIVE_BIOS_STUB_OFF_FOR(n));
	g_wr16(n * 4 + 2, NATIVE_BIOS_STUB_SEG);
}

void cpu_install_dos_handlers(CPU *cpu)
{
	(void)cpu;
	set_dos_vec(0x20, fdos_20h);
	set_dos_vec(0x21, fdos_21h);
	set_dos_vec(0x25, fdos_25h);
	set_dos_vec(0x26, fdos_26h);
	set_dos_vec(0x27, fdos_27h);
	set_dos_vec(0x28, fdos_28h);
	set_dos_vec(0x29, fdos_29h);
	set_dos_vec(0x2F, fdos_2fh);
	/* the CP/M CALL 5 gateway: 0000:00C0 jumps to the 30h stub (kernel.c) */
	handlers[0x30] = fdos_30h;
}

bool fdos_vector_is_native(uint8_t n)
{
	uint16_t off = g_rd16(n * 4), seg = g_rd16(n * 4 + 2);
	uint16_t rom = (uint16_t)(hle_ram[0xF0000u + 2 * n] | hle_ram[0xF0000u + 2 * n + 1] << 8);
	return seg == 0xF000 && (off == rom || off == NATIVE_BIOS_STUB_OFF_FOR(n));
}
