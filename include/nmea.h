/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifndef NMEA_H
#define NMEA_H

#include "gnss_view.h"

/* Multi-talker NMEA accumulator. A single stream typically interleaves
 * GPGSV / GLGSV / GBGSV / GAGSV etc.; we accumulate each talker's GSV
 * sequence independently and merge into one gnss_svinfo view. */
struct nmea_state {
	struct gnss_sv work[64];
	uint8_t work_count;
	uint8_t expect_msg[8];  /* next GSV msg_num expected per gnss_id */
	uint8_t total_msg[8];   /* total_msgs announced per gnss_id */
};

void nmea_state_init(struct nmea_state *s);

/* Verify and strip the `*XX` NMEA checksum. Returns 1 if the checksum
 * matches (XOR of chars between `$` and `*`), 0 otherwise. */
int nmea_checksum_ok(const char *line);

/* Feed one complete NMEA line (terminator already stripped). Updates
 * pvt/sv from recognized sentences. Returns a bitmask:
 *   bit 0 (NMEA_GOT_PVT) = PVT fields refreshed (caller can redraw)
 *   bit 1 (NMEA_GOT_SV)  = SV info updated */
#define NMEA_GOT_PVT 0x1
#define NMEA_GOT_SV  0x2
int nmea_feed(struct nmea_state *s, const char *line,
              struct gnss_pvt *pvt, struct gnss_svinfo *sv);

#endif
