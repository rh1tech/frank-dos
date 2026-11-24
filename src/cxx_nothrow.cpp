/*
 * frank-386 - libstdc++'s throw helpers, without the throwing.
 *
 * The PS/2 keyboard classes use `new` and std::function, and with them
 * libstdc++ links std::__throw_bad_alloc / __throw_bad_function_call
 * (functexcept.o), which bring __cxa_throw, the personality routine and the
 * whole ARM unwinder. The SDK keeps all of libgcc in SRAM, so that was ~4 KB
 * of SRAM for code that never runs: nothing here catches (C++ is built
 * without exceptions), and a throw would end in terminate() anyway. These end
 * in panic() instead, and the unwinder stays out.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pico/platform/panic.h"

namespace std {
[[noreturn]] void __throw_bad_alloc() { panic("out of memory (new)"); }
[[noreturn]] void __throw_bad_function_call() { panic("bad function call"); }
[[noreturn]] void __throw_bad_array_new_length() { panic("bad array new length"); }
[[noreturn]] void __throw_length_error(const char *what) { panic("length error: %s", what); }
}
