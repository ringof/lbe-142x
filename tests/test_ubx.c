/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#include "ubx.h"
#include "gnss_view.h"
#include "test_util.h"

#include <string.h>

static void put_u16(uint8_t *p, uint16_t v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Build a full UBX frame (sync + header + payload + Fletcher-8) into out.
 * Returns the total byte count. */
static size_t ubx_frame(uint8_t *out, uint8_t cls, uint8_t id,
                        const uint8_t *payload, uint16_t len) {
	out[0] = 0xB5;
	out[1] = 0x62;
	out[2] = cls;
	out[3] = id;
	put_u16(&out[4], len);
	if (len) memcpy(&out[6], payload, len);
	uint8_t a = 0, b = 0;
	for (size_t k = 2; k < (size_t)6 + len; k++) {
		a = (uint8_t)(a + out[k]);
		b = (uint8_t)(b + a);
	}
	out[6 + len] = a;
	out[7 + len] = b;
	return (size_t)8 + len;
}

/* Collector for clocklog_feed's emit callback: keeps the last row + a count. */
struct row_collect { char last[160]; int count; };
static void collect_row(const char *row, void *ctx) {
	struct row_collect *c = (struct row_collect *)ctx;
	snprintf(c->last, sizeof c->last, "%s", row);
	c->count++;
}

/* True if the final comma-separated CSV field of `row` is the single char `ch`
 * (used to check the trailing `gap` flag). */
static int last_field_is(const char *row, char ch) {
	size_t L = strlen(row);
	return L >= 2 && row[L - 1] == ch && row[L - 2] == ',';
}

static void make_pvt(uint8_t pl[92], uint8_t valid_flags) {
	memset(pl, 0, 92);
	put_u16(&pl[4], 2025);          /* year */
	pl[6] = 6; pl[7] = 27;          /* month, day */
	pl[8] = 12; pl[9] = 34; pl[10] = 56;  /* hh:mm:ss */
	pl[11] = valid_flags;           /* validDate(bit0) | validTime(bit1) */
	pl[20] = 3;                     /* fixType = 3D */
	pl[23] = 9;                     /* numSV */
	put_u32(&pl[24], 100000000u);   /* lon 1e7 */
	put_u32(&pl[28], 480000000u);   /* lat 1e7 */
	put_u32(&pl[36], 545400u);      /* hMSL mm */
}

void run_ubx_tests(void) {
	printf("ubx:\n");
	uint8_t frame[800];

	/* checksum: valid frame ok; corrupted payload fails; runt (<8) fails */
	{
		uint8_t pl[92];
		make_pvt(pl, 0x03);
		size_t n = ubx_frame(frame, 0x01, 0x07, pl, 92);
		CHECK(ubx_checksum_ok(frame, n) == 1);
		frame[10] ^= 0xFF;
		CHECK(ubx_checksum_ok(frame, n) == 0);
		CHECK(ubx_checksum_ok(frame, 4) == 0);
	}

	/* NAV-PVT: time_valid requires BOTH validDate+validTime (0x03) */
	{
		uint8_t flags[4]  = {0x00, 0x01, 0x02, 0x03};
		uint8_t expect[4] = {0,    0,    0,    1};
		for (int i = 0; i < 4; i++) {
			struct gnss_pvt pvt;
			uint8_t pl[92];
			memset(&pvt, 0, sizeof pvt);
			make_pvt(pl, flags[i]);
			ubx_parse_pvt(pl, 92, &pvt);
			CHECK(pvt.valid == 1);
			CHECK(pvt.time_valid == expect[i]);
		}
	}

	/* NAV-PVT: field decode, and no-op when payload too short */
	{
		struct gnss_pvt pvt;
		uint8_t pl[92];
		memset(&pvt, 0, sizeof pvt);
		make_pvt(pl, 0x03);
		ubx_parse_pvt(pl, 92, &pvt);
		CHECK(pvt.year == 2025 && pvt.month == 6 && pvt.day == 27);
		CHECK(pvt.hour == 12 && pvt.min == 34 && pvt.sec == 56);
		CHECK(pvt.fix_type == 3 && pvt.num_sv == 9);
		CHECK(pvt.lon_1e7 == 100000000 && pvt.lat_1e7 == 480000000);
		CHECK(pvt.hmsl_mm == 545400);

		memset(&pvt, 0, sizeof pvt);
		ubx_parse_pvt(pl, 50, &pvt);   /* < 92 -> ignored */
		CHECK(pvt.valid == 0);
	}

	/* NAV-SAT: field decode, svUsed bit, length clamp, >64 clamp */
	{
		struct gnss_svinfo sv;
		uint8_t pl[8 + 3 * 12];
		memset(&sv, 0, sizeof sv);
		memset(pl, 0, sizeof pl);
		pl[4] = 1;   /* version */
		pl[5] = 3;   /* numSvs */
		/* sv0: GPS svid5 cno42 elev10 azim123 used */
		pl[8] = 0; pl[9] = 5; pl[10] = 42; pl[11] = 10;
		put_u16(&pl[12], 123);
		put_u32(&pl[16], (1u << 3));        /* flags: svUsed (bit3) */
		/* sv1: GLONASS svid7 cno30 unused */
		pl[20] = 6; pl[21] = 7; pl[22] = 30;
		put_u32(&pl[28], 0);
		ubx_parse_sat(pl, (int)sizeof pl, &sv);
		CHECK(sv.valid == 1 && sv.num_ch == 3);
		CHECK(sv.sats[0].gnss_id == 0 && sv.sats[0].svid == 5 && sv.sats[0].cno == 42);
		CHECK(sv.sats[0].elev == 10 && sv.sats[0].azim == 123 && sv.sats[0].used == 1);
		CHECK(sv.sats[1].gnss_id == 6 && sv.sats[1].used == 0);

		/* claim more SVs than the payload holds -> clamp to (n-8)/12 */
		memset(&sv, 0, sizeof sv);
		pl[5] = 10;
		ubx_parse_sat(pl, (int)sizeof pl, &sv);
		CHECK(sv.num_ch == 3);
	}
	{
		static uint8_t big[8 + 70 * 12];
		struct gnss_svinfo sv;
		memset(big, 0, sizeof big);
		memset(&sv, 0, sizeof sv);
		big[4] = 1;
		big[5] = 70;   /* > 64 */
		ubx_parse_sat(big, (int)sizeof big, &sv);
		CHECK(sv.num_ch == 64);
	}

	/* NAV-CLOCK: signed bias/drift, accuracies */
	{
		struct ubx_clock clk;
		uint8_t pl[20];
		memset(&clk, 0, sizeof clk);
		memset(pl, 0, sizeof pl);
		put_u32(&pl[0],  123456000u);         /* iTOW ms */
		put_u32(&pl[4],  (uint32_t)(-1234));  /* clkB ns */
		put_u32(&pl[8],  (uint32_t)(-12));    /* clkD ns/s */
		put_u32(&pl[12], 4u);                 /* tAcc ns */
		put_u32(&pl[16], 244u);               /* fAcc ps/s */
		ubx_parse_clock(pl, 20, &clk);
		CHECK(clk.valid == 1);
		CHECK(clk.itow_ms == 123456000u);
		CHECK(clk.clkb_ns == -1234 && clk.clkd_nsps == -12);
		CHECK(clk.tacc_ns == 4u && clk.facc_ps == 244u);
	}

	/* clocklog_row: CSV format, sentinel accuracies -> -1, valid trust gate */
	{
		char row[160];
		struct ubx_clock clk = {0};
		clk.valid = 1;
		clk.itow_ms = 123456789u;   /* 123456.789 s */
		clk.clkb_ns = -1234;
		clk.clkd_nsps = -12;
		clk.tacc_ns = 4u;
		clk.facc_ps = 244u;

		/* Good 3D-fix sample with 9 sats -> valid=1, gap=0. */
		CHECK(clocklog_row(row, sizeof row, &clk, 3, 9, 0) > 0);
		CHECK(strcmp(row, "123456.789,-1234,-12,4,244,3,9,1,0") == 0);

		/* fixType < 2 (no/2D-less fix) -> valid=0 even with good accuracies. */
		CHECK(clocklog_row(row, sizeof row, &clk, 0, 0, 0) > 0);
		CHECK(strcmp(row, "123456.789,-1234,-12,4,244,0,0,0,0") == 0);

		/* gap flag is passed through. */
		CHECK(clocklog_row(row, sizeof row, &clk, 3, 9, 1) > 0);
		CHECK(strcmp(row, "123456.789,-1234,-12,4,244,3,9,1,1") == 0);

		/* Sentinel tAcc/fAcc (unknown) -> -1, and valid forced to 0. */
		clk.tacc_ns = UINT32_MAX;
		clk.facc_ps = UINT32_MAX;
		CHECK(clocklog_row(row, sizeof row, &clk, 3, 9, 0) > 0);
		CHECK(strcmp(row, "123456.789,-1234,-12,-1,-1,3,9,0,0") == 0);

		/* Too-small buffer -> 0, no overflow. */
		CHECK(clocklog_row(row, 8, &clk, 3, 9, 0) == 0);
	}

	/* clocklog_feed: iTOW continuity -- drop duplicates, flag gaps, never
	 * interpolate, emit one row per NAV-CLOCK message -- driven by synthesized
	 * NAV-CLOCK frames through the emit callback. */
	{
		struct clocklog_state st;
		clocklog_init(&st);
		struct row_collect c = {{0}, 0};
		uint8_t cpl[20];
		uint8_t cf[28];
		size_t cfn;
		memset(cpl, 0, sizeof cpl);
		put_u32(&cpl[12], 7u);

		/* First NAV-CLOCK at iTOW=1000 ms -> one row, gap=0. */
		put_u32(&cpl[0], 1000u);
		cfn = ubx_frame(cf, 0x01, 0x22, cpl, 20);
		CHECK(clocklog_feed(&st, cf, cfn, collect_row, &c) == 1);
		CHECK(c.count == 1 && strncmp(c.last, "1.000,", 6) == 0);
		CHECK(last_field_is(c.last, '0'));   /* gap=0 */

		/* Same iTOW again -> stale/duplicate, dropped (no row). */
		CHECK(clocklog_feed(&st, cf, cfn, collect_row, &c) == 0);
		CHECK(c.count == 1);

		/* iTOW advances by exactly 1 s -> row, gap=0. */
		put_u32(&cpl[0], 2000u);
		cfn = ubx_frame(cf, 0x01, 0x22, cpl, 20);
		CHECK(clocklog_feed(&st, cf, cfn, collect_row, &c) == 1);
		CHECK(strncmp(c.last, "2.000,", 6) == 0 && last_field_is(c.last, '0'));

		/* iTOW jumps 2 s (a missed second) -> row, gap=1, never interpolated. */
		put_u32(&cpl[0], 4000u);
		cfn = ubx_frame(cf, 0x01, 0x22, cpl, 20);
		CHECK(clocklog_feed(&st, cf, cfn, collect_row, &c) == 1);
		CHECK(strncmp(c.last, "4.000,", 6) == 0 && last_field_is(c.last, '1'));

		/* A keepalive (n==0) yields no row. */
		CHECK(clocklog_feed(&st, cf, 0, collect_row, &c) == 0);

		/* Two NAV-CLOCKs concatenated in ONE feed: both must emit (1 s apart),
		 * not collapse into a single row with a phantom 2 s gap. */
		struct clocklog_state st2;
		clocklog_init(&st2);
		struct row_collect c2 = {{0}, 0};
		uint8_t pair[56];
		size_t pn = 0;
		put_u32(&cpl[0], 5000u);
		pn += ubx_frame(&pair[pn], 0x01, 0x22, cpl, 20);
		put_u32(&cpl[0], 6000u);
		pn += ubx_frame(&pair[pn], 0x01, 0x22, cpl, 20);
		CHECK(clocklog_feed(&st2, pair, pn, collect_row, &c2) == 2);
		CHECK(c2.count == 2 && last_field_is(c2.last, '0'));  /* 2nd row gap=0 */
	}

	/* ubx_consume: 0xFF padding skipped + a frame reassembled across two feeds */
	{
		uint8_t buf[1024];
		size_t buf_len = 0;
		struct gnss_pvt pvt;
		struct gnss_svinfo sv;
		struct ubx_clock clk;
		uint8_t pl[92];
		uint8_t stream[256];
		size_t sn = 0;
		memset(&pvt, 0, sizeof pvt);
		memset(&sv, 0, sizeof sv);
		memset(&clk, 0, sizeof clk);
		make_pvt(pl, 0x03);

		stream[sn++] = 0xFF;            /* leading inter-message padding */
		stream[sn++] = 0xFF;
		sn += ubx_frame(&stream[sn], 0x01, 0x07, pl, 92);
		stream[sn++] = 0xFF;           /* trailing padding */

		size_t cut = 30;               /* split mid-frame */
		int up = ubx_consume(buf, &buf_len, sizeof buf, stream, cut,
		                     &pvt, &sv, &clk);
		CHECK(up == 0 && pvt.valid == 0);
		up = ubx_consume(buf, &buf_len, sizeof buf, stream + cut, sn - cut,
		                 &pvt, &sv, &clk);
		CHECK(up == 1 && pvt.valid == 1 && pvt.num_sv == 9);

		/* a NAV-CLOCK frame in a single feed */
		uint8_t cpl[20];
		uint8_t cf[28];
		size_t cfn;
		memset(cpl, 0, sizeof cpl);
		put_u32(&cpl[12], 7u);
		cfn = ubx_frame(cf, 0x01, 0x22, cpl, 20);
		ubx_consume(buf, &buf_len, sizeof buf, cf, cfn, &pvt, &sv, &clk);
		CHECK(clk.valid == 1 && clk.tacc_ns == 7u);
	}

	/* ubx_next: extract messages, skip 0xFF/0x00 padding, leave a partial tail */
	{
		uint8_t b[400];
		size_t n = 0;
		b[n++] = 0xFF; b[n++] = 0xFF;                 /* leading padding */
		uint8_t pl[20];
		memset(pl, 0, sizeof pl);
		n += ubx_frame(&b[n], 0x0A, 0x04, pl, 20);    /* msg 1: cls 0x0A id 0x04 */
		b[n++] = 0x00;                                /* inter-message padding */
		uint8_t pl2[4] = {1, 2, 3, 4};
		n += ubx_frame(&b[n], 0x06, 0x3E, pl2, 4);    /* msg 2: cls 0x06 id 0x3E */
		b[n++] = 0xB5; b[n++] = 0x62; b[n++] = 0x01;  /* truncated 3rd message */

		size_t off = 0;
		uint8_t cls, id;
		const uint8_t *p;
		uint16_t plen;
		struct ubx_scan_stats st = {0};
		CHECK(ubx_next(b, n, &off, &cls, &id, &p, &plen, &st) == 1);
		CHECK(cls == 0x0A && id == 0x04 && plen == 20);
		CHECK(ubx_next(b, n, &off, &cls, &id, &p, &plen, &st) == 1);
		CHECK(cls == 0x06 && id == 0x3E && plen == 4 && p[0] == 1 && p[3] == 4);
		CHECK(ubx_next(b, n, &off, &cls, &id, &p, &plen, &st) == 0);
		CHECK(off == n - 3);            /* partial 3rd message retained */
		CHECK(st.pad_skips == 3);       /* the 0xFF 0xFF + 0x00 */
		CHECK(st.ck_fails == 0);
	}
}
