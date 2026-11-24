/* FRANK DOS host build: what newlib and the Pico SDK give the firmware's
 * sources, forced into every host compile (-include) */
#pragma once
#include "pico.h"     /* (the SDK puts it in front of every board compile too) */
#ifndef __unreachable
#define __unreachable() __builtin_unreachable()
#endif
#ifdef __cplusplus
extern "C" {
#endif
char *strupr(char *s);
char *strlwr(char *s);
#ifdef __cplusplus
}
#endif
/* newlib's prototype spelling (kernel.c declares vsnprintf itself) */
#ifndef _ATTRIBUTE
#define _ATTRIBUTE(attrs) __attribute__(attrs)
#endif
typedef __builtin_va_list __gnuc_va_list;
