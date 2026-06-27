/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifndef LBE_TEST_UTIL_H
#define LBE_TEST_UTIL_H

#include <stdio.h>

/* Minimal no-framework test scaffolding. Each CHECK bumps a global counter
 * and, on failure, prints the location + the failing expression. The runner
 * (tests/test_main.c) returns nonzero if any check failed. */
extern int g_test_total;
extern int g_test_fail;

#define CHECK(cond)                                                       \
	do {                                                              \
		g_test_total++;                                           \
		if (!(cond)) {                                            \
			g_test_fail++;                                    \
			printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
		}                                                         \
	} while (0)

#endif /* LBE_TEST_UTIL_H */
