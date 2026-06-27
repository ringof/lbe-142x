/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
/* Replay real captured EP 0x83 diagnostics frames (the .bin files under
 * tests/fixtures, raw 64-byte HID frames from a usbmon capture of --diag on an
 * LBE-1425) through
 * the de-framer + ubx_consume, exactly as m1425_diag does, and assert the
 * reassembled NAV-PVT/SAT/CLOCK. Exercises the shared ubx_next scanner on real
 * frame boundaries and 0xFF padding. */
#include "ubx.h"
#include "gnss_view.h"
#include "test_util.h"

#include <stdio.h>
#include <stdint.h>

#ifndef LBE_FIXTURE_DIR
#define LBE_FIXTURE_DIR "tests/fixtures"
#endif

static int replay(const char *name, struct gnss_pvt *pvt,
                  struct gnss_svinfo *sv, struct ubx_clock *clk) {
	char path[512];
	snprintf(path, sizeof path, "%s/%s", LBE_FIXTURE_DIR, name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("  FAIL cannot open %s\n", path);
		g_test_total++; g_test_fail++;
		return 0;
	}
	static uint8_t frames[64 * 1024];
	size_t total = fread(frames, 1, sizeof frames, f);
	fclose(f);

	uint8_t buf[1024];
	size_t buf_len = 0;
	int updates = 0;
	for (size_t fo = 0; fo + 64 <= total; fo += 64) {
		const uint8_t *r = &frames[fo];   /* one 64-byte [seq][len][payload] frame */
		if (r[1] == 0) continue;          /* len 0 = keepalive */
		size_t payload = r[1];
		if (payload > 62) payload = 62;
		updates += ubx_consume(buf, &buf_len, sizeof buf, r + 2, payload,
		                       pvt, sv, clk);
	}
	return updates;
}

void run_replay_tests(void) {
	printf("replay:\n");

	/* No antenna: receiver has time but no position fix; coarse clock. */
	{
		struct gnss_pvt pvt = {0};
		struct gnss_svinfo sv = {0};
		struct ubx_clock clk = {0};
		int up = replay("ubx_1425_noant.bin", &pvt, &sv, &clk);
		CHECK(up > 0);
		CHECK(pvt.valid == 1 && pvt.time_valid == 1);
		CHECK(pvt.fix_type == 0 && pvt.num_sv == 0);
		CHECK(sv.valid == 1 && sv.num_ch > 0);
		CHECK(clk.valid == 1 && clk.tacc_ns > 100);
	}

	/* Antenna + 3D fix: position fix, satellites used, tight clock. */
	{
		struct gnss_pvt pvt = {0};
		struct gnss_svinfo sv = {0};
		struct ubx_clock clk = {0};
		int up = replay("ubx_1425_3dfix.bin", &pvt, &sv, &clk);
		CHECK(up > 0);
		CHECK(pvt.valid == 1 && pvt.time_valid == 1);
		CHECK(pvt.fix_type == 3 && pvt.num_sv >= 4);
		CHECK(sv.valid == 1 && sv.num_ch >= 10);
		CHECK(clk.valid == 1 && clk.tacc_ns < 100);
	}
}
