/*
 * FRANK DOS host build: what the Pico SDK and the board's drivers give the
 * firmware (the clock, timers, panic), and the guest RAM: 8 MB mapped at
 * FRANK_PSRAM_BASE, where the firmware has its PSRAM.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include <dlfcn.h>
#include <pthread.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#include <execinfo.h>
#include <sys/ucontext.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "pico/time.h"

#define HOST_PSRAM_BYTES (16u << 20)

/* before any constructor: guest_ref.hpp's and the C++ statics' may touch it */
/* a crash prints where it happened (the coroutines' stacks are in the guest RAM) */
static void on_fault(int sig, siginfo_t *si, void *uc)
{
	fprintf(stderr, "host: signal %d at address %p\n", sig, si->si_addr);
#if defined(__APPLE__) && defined(__aarch64__)
	/* the faulting frame chain: the code's own, not the handler's stack */
	ucontext_t *u = uc;
	uintptr_t pc = (uintptr_t)u->uc_mcontext->__ss.__pc, lr = (uintptr_t)u->uc_mcontext->__ss.__lr;
	uintptr_t fp = (uintptr_t)u->uc_mcontext->__ss.__fp;
	uintptr_t addrs[40];
	int n = 0;
	addrs[n++] = pc;
	addrs[n++] = lr;
	/* only frames on a stack that is there: this thread's, or a coroutine's in the guest RAM
	 * (a fault in here, with the signal blocked, would hang the process) */
	uintptr_t st_hi = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
	uintptr_t st_lo = st_hi - pthread_get_stacksize_np(pthread_self());
#define FRAME_OK(p) (((p) >= st_lo && (p) + 16 <= st_hi) || \
	((p) >= (uintptr_t)FRANK_PSRAM_BASE && (p) + 16 <= (uintptr_t)FRANK_PSRAM_BASE + HOST_PSRAM_BYTES))
	while (fp && n < 40 && !(fp & 7) && FRAME_OK(fp)) {
		uintptr_t *f = (uintptr_t *)fp;
		uintptr_t ret = f[1] & 0x0000FFFFFFFFFFFFull;
		if (!ret)
			break;
		addrs[n++] = ret;
		if (f[0] <= fp)
			break;
		fp = f[0];
	}
	/* raw addresses only (no locks in here): tools/host_crash.sh symbolizes them */
	for (int i = 0; i < n; i++) {
		char line[40];
		uintptr_t v = addrs[i] & 0x0000FFFFFFFFFFFFull;
		int k = 0;
		line[k++] = ' ';
		line[k++] = '0';
		line[k++] = 'x';
		for (int sh = 44; sh >= 0; sh -= 4)
			line[k++] = "0123456789abcdef"[(v >> sh) & 15];
		line[k++] = '\n';
		write(2, line, (size_t)k);
	}
	{   /* the stack where it went wrong: return addresses there name the callers */
		uintptr_t sp = (uintptr_t)u->uc_mcontext->__ss.__sp;
		fprintf(stderr, "  sp %p fp %p\n", (void *)sp, (void *)u->uc_mcontext->__ss.__fp);
		for (uintptr_t p = sp - 64; p < sp + 512; p += 8)
			if (FRAME_OK(p))
				fprintf(stderr, "  [sp%+4d] %016lx\n", (int)(p - sp), (unsigned long)*(uintptr_t *)p);
	}
	Dl_info self;
	if (dladdr((void *)on_fault, &self))
		fprintf(stderr, "  (image base %p)\n", self.dli_fbase);
#else
	(void)uc;
	void *bt[48];
	backtrace_symbols_fd(bt, backtrace(bt, 48), 2);
#endif
	_exit(128 + sig);
}

__attribute__((constructor(101))) static void host_map_psram(void)
{
	static char altstack[64 << 10];
	stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
	sigaltstack(&ss, NULL);
	struct sigaction sa = { 0 };
	sa.sa_sigaction = on_fault;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;   /* (a fault in here ends it) */
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);

	void *want = (void *)(uintptr_t)FRANK_PSRAM_BASE;
	/* MAP_FIXED replaces whatever is there: make sure nothing is */
#ifdef __APPLE__
	mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)want;
	mach_vm_size_t sz;
	vm_region_basic_info_data_64_t info;
	mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t obj;
	if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) == KERN_SUCCESS &&
	    a < (mach_vm_address_t)(uintptr_t)want + HOST_PSRAM_BYTES) {
		fprintf(stderr, "host: the guest RAM's address %p is in use\n", want);
		exit(1);
	}
#endif
	void *p = mmap(want, HOST_PSRAM_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);   /* (macOS ignores a plain hint; nothing is there this early) */
	if (p != want) {
		fprintf(stderr, "host: cannot map the guest RAM at %p (got %p)\n", want, p);
		exit(1);
	}
}

uint64_t time_us_64(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void sleep_us(uint64_t us)
{
	usleep((useconds_t)us);
}

void panic(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "PANIC: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	abort();
}

/* ---- repeating timers, run from the main loop ------------------------------- */
#define MAX_TIMERS 4
static repeating_timer_t *timers[MAX_TIMERS];

bool add_repeating_timer_us(int64_t delay_us, repeating_timer_callback_t cb, void *user, repeating_timer_t *out)
{
	for (int i = 0; i < MAX_TIMERS; i++)
		if (!timers[i]) {
			out->delay_us = delay_us < 0 ? -delay_us : delay_us;
			out->callback = cb;
			out->user_data = user;
			out->next = time_us_64() + (uint64_t)out->delay_us;
			timers[i] = out;
			return true;
		}
	return false;
}

bool cancel_repeating_timer(repeating_timer_t *t)
{
	for (int i = 0; i < MAX_TIMERS; i++)
		if (timers[i] == t) {
			timers[i] = NULL;
			return true;
		}
	return false;
}

void host_timers_run(void)
{
	uint64_t now = time_us_64();
	for (int i = 0; i < MAX_TIMERS; i++) {
		repeating_timer_t *t = timers[i];
		if (!t)
			continue;
		int n = 0;
		while (now >= t->next && n++ < 64) {
			t->next += (uint64_t)t->delay_us;
			if (!t->callback(t)) {
				timers[i] = NULL;
				break;
			}
		}
		if (now >= t->next)
			t->next = now + (uint64_t)t->delay_us;
	}
}

size_t psram_usable_size(void) { return 8u << 20; }

char *strupr(char *s)
{
	for (char *p = s; *p; p++)
		if (*p >= 'a' && *p <= 'z')
			*p -= 32;
	return s;
}

char *strlwr(char *s)
{
	for (char *p = s; *p; p++)
		if (*p >= 'A' && *p <= 'Z')
			*p += 32;
	return s;
}
uint32_t host_sysinfo[4];
