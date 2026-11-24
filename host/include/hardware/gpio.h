/* FRANK DOS host build: no GPIO */
#ifndef FRANK_HOST_GPIO_H
#define FRANK_HOST_GPIO_H
#include "pico.h"
#define GPIO_OUT 1
#define GPIO_IN 0
static inline void gpio_init(uint g) { (void)g; }
static inline void gpio_set_dir(uint g, bool o) { (void)g; (void)o; }
static inline void gpio_put(uint g, bool v) { (void)g; (void)v; }
static inline bool gpio_get(uint g) { (void)g; return false; }
static inline void gpio_pull_up(uint g) { (void)g; }
#endif
