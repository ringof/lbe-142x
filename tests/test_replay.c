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

/* Replay a fixture through the clocklog pipeline, exactly as m1425_clocklog
 * does, collecting per-row honesty stats from the emitted CSV. */
struct clocklog_replay_stats {
	int rows;        /* total NAV-CLOCK rows emitted */
	int valid_rows;  /* rows with the trust-gate valid flag set */
	int fix3_rows;   /* rows reporting a 3D fix */
	int gap_rows;    /* rows flagged with an explicit iTOW gap */
	int itow_monotonic;  /* 1 if iTOW never went backwards */
};

struct clocklog_replay_ctx {
	struct clocklog_replay_stats *s;
	double last_itow;
};

static void clocklog_replay_emit(const char *row, void *vctx) {
	struct clocklog_replay_ctx *c = (struct clocklog_replay_ctx *)vctx;
	double itow; int clkb, clkd; long ta, fa; int fix, nsv, val, gap;
	if (sscanf(row, "%lf,%d,%d,%ld,%ld,%d,%d,%d,%d",
	           &itow, &clkb, &clkd, &ta, &fa, &fix, &nsv, &val, &gap) != 9)
		return;
	c->s->rows++;
	if (val) c->s->valid_rows++;
	if (fix == 3) c->s->fix3_rows++;
	if (gap) c->s->gap_rows++;
	if (c->last_itow >= 0 && itow < c->last_itow) c->s->itow_monotonic = 0;
	c->last_itow = itow;
}

static void clocklog_replay(const char *name,
                            struct clocklog_replay_stats *out) {
	char path[512];
	snprintf(path, sizeof path, "%s/%s", LBE_FIXTURE_DIR, name);
	out->rows = out->valid_rows = out->fix3_rows = out->gap_rows = 0;
	out->itow_monotonic = 1;
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("  FAIL cannot open %s\n", path);
		g_test_total++; g_test_fail++;
		out->itow_monotonic = 0;
		return;
	}
	static uint8_t frames[64 * 1024];
	size_t total = fread(frames, 1, sizeof frames, f);
	fclose(f);

	struct clocklog_state st;
	clocklog_init(&st);
	struct clocklog_replay_ctx ctx = { out, -1.0 };
	for (size_t fo = 0; fo + 64 <= total; fo += 64) {
		const uint8_t *r = &frames[fo];   /* [seq][len][payload] */
		size_t payload = (r[1] == 0) ? 0 : r[1];
		if (payload > 62) payload = 62;
		clocklog_feed(&st, r + 2, payload, clocklog_replay_emit, &ctx);
	}
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

	/* clocklog over the same fixtures: honest CSV rows, iTOW-driven. */
	{
		/* No antenna: no fix -> every row's trust gate must read invalid. */
		struct clocklog_replay_stats s;
		clocklog_replay("ubx_1425_noant.bin", &s);
		CHECK(s.rows > 0);
		CHECK(s.valid_rows == 0);     /* no fix -> nothing trustworthy */
		CHECK(s.fix3_rows == 0);
		CHECK(s.itow_monotonic == 1); /* iTOW never goes backwards */
		CHECK(s.gap_rows == 0);       /* clean 1 Hz capture -> no false gaps */
	}
	{
		/* 3D fix: at least some rows pass the trust gate and report fix 3. */
		struct clocklog_replay_stats s;
		clocklog_replay("ubx_1425_3dfix.bin", &s);
		CHECK(s.rows > 0);
		CHECK(s.valid_rows > 0);
		CHECK(s.fix3_rows > 0);
		CHECK(s.itow_monotonic == 1);
		CHECK(s.gap_rows == 0);       /* clean capture -> no spurious gaps */
	}
}
