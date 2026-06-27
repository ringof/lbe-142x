/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
/* Hardware-free unit tests for the pure parsing units (src/ubx.c, src/nmea.c).
 * Run via `ctest` or directly; exits nonzero if any check fails. */
#include <stdio.h>

int g_test_total = 0;
int g_test_fail = 0;

void run_ubx_tests(void);
void run_nmea_tests(void);
void run_replay_tests(void);

int main(void) {
	run_ubx_tests();
	run_nmea_tests();
	run_replay_tests();
	printf("\n%d checks, %d failed\n", g_test_total, g_test_fail);
	return g_test_fail ? 1 : 0;
}
