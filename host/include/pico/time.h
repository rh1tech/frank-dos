/* FRANK DOS host build: pico/time.h on the host clock. */
#ifndef FRANK_HOST_PICO_TIME_H
#define FRANK_HOST_PICO_TIME_H
#include "pico.h"
#ifdef __cplusplus
extern "C" {
#endif
uint64_t time_us_64(void);
static inline uint32_t time_us_32(void) { return (uint32_t)time_us_64(); }
void sleep_us(uint64_t us);
static inline void sleep_ms(uint32_t ms) { sleep_us((uint64_t)ms * 1000); }
static inline void busy_wait_us(uint64_t us) { sleep_us(us); }
static inline void busy_wait_us_32(uint32_t us) { sleep_us(us); }
static inline void busy_wait_ms(uint32_t ms) { sleep_ms(ms); }

typedef uint64_t absolute_time_t;
static inline absolute_time_t get_absolute_time(void) { return time_us_64(); }
static inline uint64_t to_us_since_boot(absolute_time_t t) { return t; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000); }
static inline int64_t absolute_time_diff_us(absolute_time_t a, absolute_time_t b) { return (int64_t)(b - a); }
static inline absolute_time_t make_timeout_time_ms(uint32_t ms) { return time_us_64() + (uint64_t)ms * 1000; }

typedef struct repeating_timer repeating_timer_t;
typedef bool (*repeating_timer_callback_t)(repeating_timer_t *rt);
struct repeating_timer {
	int64_t delay_us;
	repeating_timer_callback_t callback;
	void *user_data;
	uint64_t next;
};
/* the host runs them from its main loop (host_timers_run) */
bool add_repeating_timer_us(int64_t delay_us, repeating_timer_callback_t cb, void *user, repeating_timer_t *out);
bool cancel_repeating_timer(repeating_timer_t *t);
void host_timers_run(void);
#ifdef __cplusplus
}
#endif
#endif
