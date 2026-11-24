/* FRANK DOS host build: the PSRAM is platform.c's mapping */
#pragma once
#include "pico.h"
size_t psram_usable_size(void);
static inline size_t psram_get_size(void) { return psram_usable_size(); }
