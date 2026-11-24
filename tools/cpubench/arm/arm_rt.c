/* cpubench on a Cortex-M33 under qemu (mps2-an505): reset, semihosting I/O,
 * and a clock.  With -icount shift=0 virtual time advances 1 ns per executed
 * instruction; SysTick runs from the 25 MHz system clock, so 40 ns a tick. */
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>

extern int main(void);
extern char __bss_start__[], __bss_end__[], end[], __stack_top[];

#define SYST_CSR (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018)
#define CPACR    (*(volatile uint32_t *)0xE000ED88)

static volatile uint32_t wraps;

static int semi(int op, void *arg)
{
	register int r0 __asm__("r0") = op;
	register void *r1 __asm__("r1") = arg;
	__asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
	return r0;
}

void _exit(int code)
{
	uint32_t a[2] = {0x20026, (uint32_t)code};     /* ADP_Stopped_ApplicationExit */
	for (;;)
		semi(0x20, a);                             /* SYS_EXIT_EXTENDED */
}

int _write(int fd, const char *p, int n)
{
	uint32_t a[3] = {fd == 2 ? 2 : 1, (uint32_t)p, (uint32_t)n};
	(void)fd;
	static int h = -1;
	if (h < 0) {
		uint32_t o[3] = {(uint32_t)":tt", 4, 3};   /* stdout */
		h = semi(0x01, o);
	}
	a[0] = h;
	semi(0x05, a);
	return n;
}

void *_sbrk(int inc)
{
	static char *brk = end;
	char *p = brk;
	if (brk + inc > __stack_top - 65536) { errno = ENOMEM; return (void *)-1; }
	brk += inc;
	return p;
}
int _close(int f) { (void)f; return -1; }
int _fstat(int f, struct stat *st) { (void)f; st->st_mode = S_IFCHR; return 0; }
int _isatty(int f) { (void)f; return 1; }
int _lseek(int f, int o, int w) { (void)f; (void)o; (void)w; return 0; }
int _read(int f, char *p, int n) { (void)f; (void)p; (void)n; return 0; }
int _kill(int p, int s) { (void)p; (void)s; return -1; }
int _getpid(void) { return 1; }

#ifdef ARM_PROF
/* a sample of the interrupted PC every PERIOD ticks (tools/cpubench/run.sh prof) */
#define PERIOD 997u
#define NSAMP 200000
static uint32_t samples[NSAMP];
static uint32_t nsamp;
void systick_c(uint32_t *frame)
{
	wraps++;
	if (nsamp < NSAMP)
		samples[nsamp++] = frame[6];
}
__attribute__((naked)) void systick(void)
{
	__asm__ volatile("mrs r0, msp\n\tb systick_c");
}
void arm_prof_dump(void)
{
	printf("PROF %lu\n", (unsigned long)nsamp);
	for (uint32_t i = 0; i < nsamp; i++)
		printf("S %08lx\n", (unsigned long)samples[i]);
}
#else
#define PERIOD 0x1000000u
void systick(void) { wraps++; }
void arm_prof_dump(void) {}
#endif

uint64_t arm_time_ns(void)
{
	uint32_t w, v;
	do {
		w = wraps;
		v = SYST_CVR;
	} while (w != wraps);
	return ((uint64_t)w * PERIOD + (PERIOD - 1 - v)) * 40u;
}

void arm_init(void)
{
	SYST_RVR = PERIOD - 1;
	SYST_CVR = 0;
	SYST_CSR = 7;           /* processor clock, interrupt, enable */
}

void reset(void)
{
	CPACR |= 0xF << 20;     /* the FPU (softfp ABI, single precision in hardware) */
	__asm__ volatile("dsb; isb");
	memset(__bss_start__, 0, __bss_end__ - __bss_start__);
	_exit(main());
}

void fault_c(uint32_t *frame)
{
	printf("fault: pc %08lx lr %08lx\n", (unsigned long)frame[6], (unsigned long)frame[5]);
	_exit(99);
}

__attribute__((naked)) static void fault(void)
{
	__asm__ volatile("mrs r0, msp\n\tb fault_c");
}

__attribute__((section(".vectors"), used))
const void *const vectors[16] = {
	__stack_top, reset, fault, fault, fault, fault, fault, 0, 0, 0, 0, fault, 0, 0, fault, systick,
};

/* a host file over semihosting: handle or -1; reads return the bytes read */
int arm_fopen(const char *path)
{
	uint32_t a[3] = {(uint32_t)path, 1, (uint32_t)strlen(path)};   /* "rb" */
	return semi(0x01, a);
}
unsigned arm_fread(int h, void *buf, unsigned n)
{
	uint32_t a[3] = {(uint32_t)h, (uint32_t)buf, n};
	return n - (unsigned)semi(0x06, a);
}
void arm_fclose(int h)
{
	uint32_t a[1] = {(uint32_t)h};
	semi(0x02, a);
}

int usleep(unsigned us) { (void)us; return 0; }
int _open(const char *p, int f, int m) { (void)p; (void)f; (void)m; return -1; }
void _init(void) {}
void _fini(void) {}
