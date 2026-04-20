/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifndef LBE_PLATFORM_H
#define LBE_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* Platform-abstraction layer for OS/console primitives. All
 * #ifdef _WIN32 / __linux__ / _MSC_VER branches for these operations
 * live in src/lbe_platform.c; callers stay portable. */

void lbe_sleep_ms(int ms);

/* Monotonic millisecond counter. Wraps every ~49 days; only use for
 * short-interval deltas. */
uint32_t lbe_millis(void);

/* Copy env var `name` into `out` (NUL-terminated). Returns 1 if set
 * and non-empty, 0 otherwise. */
int  lbe_getenv(const char *name, char *out, size_t n);

void lbe_setenv(const char *name, const char *value);

/* Enable ANSI escapes in the current console (no-op where unneeded). */
void lbe_enable_vt(void);

#endif
