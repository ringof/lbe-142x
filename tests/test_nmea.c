/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#include "nmea.h"
#include "gnss_view.h"
#include "test_util.h"

#include <string.h>

void run_nmea_tests(void) {
	printf("nmea:\n");

	/* checksum: canonical GGA (*47) ok; one-off (*48) rejected; non-$ rejected */
	{
		const char *good = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
		const char *bad  = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48";
		CHECK(nmea_checksum_ok(good) == 1);
		CHECK(nmea_checksum_ok(bad) == 0);
		CHECK(nmea_checksum_ok("no dollar") == 0);
	}

	/* GGA: time, fix from quality, sat count, position, altitude.
	 * (Lines are fed without a *CS; nmea_feed tolerates a missing checksum.) */
	{
		struct nmea_state st;
		struct gnss_pvt pvt;
		struct gnss_svinfo sv;
		int r;
		nmea_state_init(&st);
		memset(&pvt, 0, sizeof pvt);
		memset(&sv, 0, sizeof sv);
		r = nmea_feed(&st,
		    "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,",
		    &pvt, &sv);
		CHECK((r & NMEA_GOT_PVT) != 0);
		CHECK(pvt.hour == 12 && pvt.min == 35 && pvt.sec == 19);
		CHECK(pvt.fix_type == 3 && pvt.time_valid == 1 && pvt.num_sv == 8);
		CHECK(pvt.lat_1e7 > 481100000 && pvt.lat_1e7 < 481200000);  /* ~48.1173 N */
		CHECK(pvt.lon_1e7 > 112000000 && pvt.lon_1e7 < 116000000);  /* ~11.5167 E */
		CHECK(pvt.hmsl_mm == 545400);

		/* quality 0 -> no fix, time not trustworthy */
		memset(&pvt, 0, sizeof pvt);
		nmea_feed(&st,
		    "$GPGGA,123519,4807.038,N,01131.000,E,0,00,0.9,545.4,M,46.9,M,,",
		    &pvt, &sv);
		CHECK(pvt.fix_type == 0 && pvt.time_valid == 0);
	}

	/* RMC: status 'A' sets date + time_valid; 'V', an empty field, and any
	 * other status are ignored (issue #3). */
	{
		struct nmea_state st;
		struct gnss_pvt pvt;
		struct gnss_svinfo sv;
		int r;
		nmea_state_init(&st);
		memset(&pvt, 0, sizeof pvt);
		memset(&sv, 0, sizeof sv);
		r = nmea_feed(&st,
		    "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W",
		    &pvt, &sv);
		CHECK((r & NMEA_GOT_PVT) != 0);
		CHECK(pvt.time_valid == 1);
		CHECK(pvt.day == 23 && pvt.month == 3 && pvt.year == 2094);

		memset(&pvt, 0, sizeof pvt);
		r = nmea_feed(&st,
		    "$GPRMC,123519,V,4807.038,N,01131.000,E,,,230394,,",
		    &pvt, &sv);
		CHECK(r == 0 && pvt.time_valid == 0 && pvt.valid == 0);

		/* empty status field (truncated/garbled) must NOT be treated as a fix */
		memset(&pvt, 0, sizeof pvt);
		r = nmea_feed(&st,
		    "$GPRMC,123519,,4807.038,N,01131.000,E,,,230394,,",
		    &pvt, &sv);
		CHECK(r == 0 && pvt.time_valid == 0 && pvt.valid == 0);
	}

	/* GSV: a single-message sequence commits one satellite */
	{
		struct nmea_state st;
		struct gnss_pvt pvt;
		struct gnss_svinfo sv;
		int r;
		nmea_state_init(&st);
		memset(&pvt, 0, sizeof pvt);
		memset(&sv, 0, sizeof sv);
		r = nmea_feed(&st, "$GPGSV,1,1,01,05,40,083,42", &pvt, &sv);
		CHECK((r & NMEA_GOT_SV) != 0);
		CHECK(sv.valid == 1 && sv.num_ch == 1);
		CHECK(sv.sats[0].gnss_id == 0 && sv.sats[0].svid == 5 && sv.sats[0].cno == 42);
	}
}
