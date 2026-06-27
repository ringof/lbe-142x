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
		put_u32(&pl[4],  (uint32_t)(-1234));  /* clkB ns */
		put_u32(&pl[8],  (uint32_t)(-12));    /* clkD ns/s */
		put_u32(&pl[12], 4u);                 /* tAcc ns */
		put_u32(&pl[16], 244u);               /* fAcc ps/s */
		ubx_parse_clock(pl, 20, &clk);
		CHECK(clk.valid == 1);
		CHECK(clk.clkb_ns == -1234 && clk.clkd_nsps == -12);
		CHECK(clk.tacc_ns == 4u && clk.facc_ps == 244u);
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
}
