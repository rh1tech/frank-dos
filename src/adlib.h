#ifndef ADLIB_H
#define ADLIB_H

#include <stdint.h>

#define FLOAT float

#define ADLIB_BATCH_SIZE 128

typedef struct AdlibState AdlibState;

void adlib_write(void *opaque, uint32_t nport, uint32_t val);
uint32_t adlib_read(void *opaque, uint32_t nport);
AdlibState *adlib_new();
// call it 44100 times per sec from timer on core1 (ISR, so should be fast)
int16_t adlib_getsample(AdlibState *s);
#if ADLIB_CORE1
/* the synth, a slice at a time, from core 1's audio loop (adlib.c) */
void adlib_core1_poll(AdlibState *s);
#endif

#endif /* ADLIB_H */
