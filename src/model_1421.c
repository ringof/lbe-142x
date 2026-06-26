/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#include "lbe_common.h"
#include "lbe_model.h"
#include "lbe_transport.h"
#include "lbe_serial.h"
#include "lbe_platform.h"
#include "nmea.h"
#include "gnss_view.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same wire convention as LBE-1420: opcode doubles as the HID Report ID
 * and the USB payload is {opcode, args, ...}. Status reads use the
 * dedicated status Report ID 0x4B. The 1421 layout differs from the
 * 1420 layout in the status report (frequency2, per-output power) and
 * in the frequency commands (u32 at buf[5..8] rather than buf[1..4]). */
#define LBE_STATUS_REPORT_ID 0x4B

static int send_cmd(struct lbe_transport *t, uint8_t opcode,
                    const uint8_t *args, size_t args_offset, size_t n) {
	uint8_t buf[LBE_REPORT_SIZE] = {0};
	buf[0] = opcode;
	if (args && n) {
		size_t room = LBE_REPORT_SIZE - args_offset;
		memcpy(&buf[args_offset], args, n > room ? room : n);
	}
	return lbe_transport_feat_set(t, opcode, buf);
}

static int m1421_get_status(struct lbe_transport *t, struct lbe_status *s) {
	uint8_t buf[LBE_REPORT_SIZE] = {0};
	if (lbe_transport_feat_get(t, LBE_STATUS_REPORT_ID, buf) < 0) return -1;

	s->raw_status     = buf[1];
	s->frequency1     = buf[6]  | (buf[7]  << 8) | (buf[8]  << 16) | (buf[9]  << 24);
	s->frequency2     = buf[14] | (buf[15] << 8) | (buf[16] << 16) | (buf[17] << 24);
	s->fll_enabled    = buf[18] != 0;
	s->out1_power_low = buf[19] != 0;
	s->out2_power_low = buf[20] != 0;
	s->out1_drive_ma  = 0;
	s->outputs_enabled = (s->raw_status & (LBE_OUT1_EN_BIT | LBE_OUT2_EN_BIT))
	                  == (LBE_OUT1_EN_BIT | LBE_OUT2_EN_BIT);
	s->pll_locked = (s->raw_status & LBE_PLL_LOCK_BIT) != 0;
	s->antenna_ok = (s->raw_status & LBE_ANT_OK_BIT) != 0;
	s->pps_enabled = (s->raw_status & LBE_PPS_EN_BIT) != 0;
	return 0;
}

static int m1421_set_frequency_op(struct lbe_transport *t, uint8_t opcode, uint32_t hz) {
	uint8_t args[4] = {
		(uint8_t)(hz),
		(uint8_t)(hz >> 8),
		(uint8_t)(hz >> 16),
		(uint8_t)(hz >> 24),
	};
	/* 1421 places the u32 at byte offset 5 of the payload, not 1. */
	return send_cmd(t, opcode, args, 5, sizeof args);
}

static int m1421_set_frequency(struct lbe_transport *t, int output, uint32_t hz) {
	uint8_t opcode;
	if (output == 1)      opcode = LBE_1421_SET_F1;
	else if (output == 2) opcode = LBE_1421_SET_F2;
	else { fprintf(stderr, "Invalid output selection\n"); return -1; }
	return m1421_set_frequency_op(t, opcode, hz);
}

static int m1421_set_frequency_temp(struct lbe_transport *t, int output, uint32_t hz) {
	uint8_t opcode;
	if (output == 1)      opcode = LBE_1421_SET_F1_TEMP;
	else if (output == 2) opcode = LBE_1421_SET_F2_TEMP;
	else { fprintf(stderr, "Invalid output selection\n"); return -1; }
	return m1421_set_frequency_op(t, opcode, hz);
}

static int m1421_set_outputs_enable(struct lbe_transport *t, int enable) {
	/* 0x03 = both outputs on; 0x00 = both off. */
	uint8_t arg = enable ? 0x03 : 0x00;
	return send_cmd(t, LBE_142X_EN_OUT, &arg, 1, 1);
}

static int m1421_blink_leds(struct lbe_transport *t) {
	return send_cmd(t, LBE_142X_BLINK_OUT, NULL, 1, 0);
}

static int m1421_set_pll_mode(struct lbe_transport *t, int fll_mode) {
	uint8_t arg = fll_mode ? 0x01 : 0x00;
	return send_cmd(t, LBE_142X_SET_PLL, &arg, 1, 1);
}

static int m1421_set_1pps(struct lbe_transport *t, int enable) {
	uint8_t arg = enable ? 0x01 : 0x00;
	return send_cmd(t, LBE_1421_SET_PPS, &arg, 1, 1);
}

static int m1421_set_power_level(struct lbe_transport *t, int output, int low) {
	uint8_t opcode;
	if (output == 1)      opcode = LBE_1421_SET_PWR1;
	else if (output == 2) opcode = LBE_1421_SET_PWR2;
	else { fprintf(stderr, "Invalid output selection\n"); return -1; }
	uint8_t arg = low ? 0x01 : 0x00;
	return send_cmd(t, opcode, &arg, 1, 1);
}

/* --- LBE-1425-only commands ---------------------------------------------
 * Same SET_REPORT feature-report transport as the rest of the 1421 family
 * (opcode in payload byte 0, arg in byte 1). Confirmed against a vendor-tool
 * USB capture; see docs/reverse/LBE-1425-config-v1.10.md. Wired into
 * lbe_ops_1425 only, so 1421/1423 never emit these opcodes (which would mean
 * something else on those models). */
static int m1425_set_gnss(struct lbe_transport *t, uint8_t mask) {
	return send_cmd(t, LBE_1425_SET_GNSS, &mask, 1, 1);
}

static int m1425_set_dynmodel(struct lbe_transport *t, uint8_t model) {
	return send_cmd(t, LBE_1425_SET_DYNMODEL, &model, 1, 1);
}

static int m1425_set_nmea(struct lbe_transport *t, int enable) {
	uint8_t arg = enable ? 0x01 : 0x00;
	return send_cmd(t, LBE_1425_SET_NMEA, &arg, 1, 1);
}

/* Live chronometer-style NMEA + 1PPS monitor. The 1421 streams GPS
 * data over USB CDC (not HID) and drives the u-blox 1PPS onto the
 * CDC DCD line. Port selection: LBE_PORT env var first, else first
 * COM/ttyACM* whose stream starts with "$G".
 *
 * Design: read NMEA with a short timeout (20 ms) so we poll DCD at
 * ~50 Hz. Redraw at ~20 Hz, interpolating sub-second UTC from
 * (now - last_edge). A ring of recent intervals feeds jitter stats. */
#define PPS_HISTORY     30
#define REDRAW_PERIOD   50   /* ms between screen redraws (~20 Hz) */
#define READ_TIMEOUT    20   /* ms; caps DCD poll period */

struct pps_tracker {
	int      dcd_prev;
	uint32_t edge_ms;           /* lbe_millis of last rising edge */
	uint32_t edges;
	uint32_t hist[PPS_HISTORY]; /* recent intervals in ms, circular */
	uint32_t hist_n;            /* total intervals pushed (>= edges-1) */
	uint32_t last_nmea_ms;      /* last NMEA arrival (for NMEA->edge latency) */
	uint32_t nmea_to_edge_ms;   /* latency of most recent edge */
};

static void pps_on_edge(struct pps_tracker *p, uint32_t now) {
	if (p->edges > 0) {
		p->hist[p->hist_n % PPS_HISTORY] = now - p->edge_ms;
		p->hist_n++;
	}
	if (p->last_nmea_ms) p->nmea_to_edge_ms = now - p->last_nmea_ms;
	p->edge_ms = now;
	p->edges++;
}

static void pps_stats(const struct pps_tracker *p,
                      uint32_t *avg, uint32_t *min, uint32_t *max,
                      uint32_t *samples) {
	uint32_t n = p->hist_n < PPS_HISTORY ? p->hist_n : PPS_HISTORY;
	*samples = n;
	if (n == 0) { *avg = *min = *max = 0; return; }
	uint32_t mn = UINT32_MAX, mx = 0, sum = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t v = p->hist[i];
		if (v < mn) mn = v;
		if (v > mx) mx = v;
		sum += v;
	}
	*avg = sum / n;
	*min = mn;
	*max = mx;
}

static int m1421_monitor(struct lbe_transport *t) {
	(void)t;
	char port[64] = {0};
	if (!lbe_getenv("LBE_PORT", port, sizeof port) &&
	    lbe_serial_find_nmea(port, sizeof port) < 0) {
		fprintf(stderr, "No NMEA serial port found. Use --port COMxx (Windows) "
		                "or --port /dev/ttyACM0 (Linux).\n");
		return -1;
	}

	struct lbe_serial *s = lbe_serial_open(port);
	if (!s) return -1;

	/* main.c passes the concrete model ("1425 dual output", ...) via env;
	 * fall back to the family name when run standalone. */
	char model_name[64];
	char title[96];
	if (!lbe_getenv("LBE_MODEL_NAME", model_name, sizeof model_name))
		snprintf(title, sizeof title, "LBE-142x GPS Monitor");
	else
		snprintf(title, sizeof title, "LBE-%s GPS Monitor", model_name);

	struct gnss_pvt pvt = {0};
	struct gnss_svinfo sv = {0};
	struct nmea_state st;
	nmea_state_init(&st);
	struct pps_tracker pps = {0};
	pps.dcd_prev = lbe_serial_get_dcd(s);

	lbe_enable_vt();
	printf("\033[2J\033[H");
	printf("Opening %s and waiting for NMEA...\n", port);
	fflush(stdout);

	char dbg_buf[2];
	int debug = lbe_getenv("LBE_NMEA_DEBUG", dbg_buf, sizeof dbg_buf);

	int lines = 0;
	int bad_ck = 0;
	uint32_t last_redraw = 0;

	for (;;) {
		int dcd = lbe_serial_get_dcd(s);
		if (dcd >= 0 && pps.dcd_prev == 0 && dcd == 1) {
			pps_on_edge(&pps, lbe_millis());
		}
		if (dcd >= 0) pps.dcd_prev = dcd;

		char line[256];
		int r = lbe_serial_readline(s, line, sizeof line, READ_TIMEOUT);
		if (r < 0) { fprintf(stderr, "serial read failed\n"); break; }
		if (r > 0) {
			lines++;
			pps.last_nmea_ms = lbe_millis();
			if (debug) fprintf(stderr, "%s\n", line);
			const char *star = strchr(line, '*');
			if (star && !nmea_checksum_ok(line)) {
				bad_ck++;
			} else {
				(void)nmea_feed(&st, line, &pvt, &sv);
			}
		}

		uint32_t now = lbe_millis();
		if (now - last_redraw < REDRAW_PERIOD) continue;
		last_redraw = now;

		int frac_ms = -1;
		if (pps.edges > 0) {
			uint32_t f = now - pps.edge_ms;
			if (f > 999) f = 999;  /* cap if PPS stalls */
			frac_ms = (int)f;
		}
		gnss_draw(title, &pvt, &sv, frac_ms);

		if (pps.edges == 0) {
			printf("\nPPS:    waiting for DCD edge...\033[K\n");
		} else {
			uint32_t avg, mn, mx, n;
			pps_stats(&pps, &avg, &mn, &mx, &n);
			if (n == 0) {
				printf("\nPPS:    edges=%" PRIu32 "  waiting for interval samples..."
				       "\033[K\n", pps.edges);
			} else {
				printf("\nPPS:    edges=%" PRIu32 "  avg=%" PRIu32 " ms  "
				       "min=%" PRIu32 "  max=%" PRIu32 "  (last %" PRIu32
				       ")  NMEA->edge=%" PRIu32 " ms\033[K\n",
				       pps.edges, avg, mn, mx, n, pps.nmea_to_edge_ms);
			}
		}
		printf("Stream: lines=%d bad_ck=%d  port=%s\033[K\n\033[J",
		       lines, bad_ck, port);
		fflush(stdout);
	}

	lbe_serial_close(s);
	return 0;
}

const struct lbe_model_ops lbe_ops_1421 = {
	.name               = "1421 dual output",
	.max_freq           = LBE_1421_MAX_FREQ,
	.init               = NULL,
	.get_status         = m1421_get_status,
	.set_frequency      = m1421_set_frequency,
	.set_frequency_temp = m1421_set_frequency_temp,
	.set_outputs_enable = m1421_set_outputs_enable,
	.blink_leds         = m1421_blink_leds,
	.set_pll_mode       = m1421_set_pll_mode,
	.set_1pps           = m1421_set_1pps,
	.set_power_level    = m1421_set_power_level,
	.monitor            = m1421_monitor,
};

/* LBE-1425: the 1421 dual-output protocol verbatim, plus the 1425's extra
 * GNSS / dynamic-model / NMEA-output commands. (Per-output frequency caps
 * are enforced separately in lbe_device.c via the PID.) */
const struct lbe_model_ops lbe_ops_1425 = {
	.name               = "1425 dual output",
	.max_freq           = LBE_1425_OUT2_MAX_FREQ,
	.init               = NULL,
	.get_status         = m1421_get_status,
	.set_frequency      = m1421_set_frequency,
	.set_frequency_temp = m1421_set_frequency_temp,
	.set_outputs_enable = m1421_set_outputs_enable,
	.blink_leds         = m1421_blink_leds,
	.set_pll_mode       = m1421_set_pll_mode,
	.set_1pps           = m1421_set_1pps,
	.set_power_level    = m1421_set_power_level,
	.monitor            = m1421_monitor,
	.set_gnss           = m1425_set_gnss,
	.set_dynmodel       = m1425_set_dynmodel,
	.set_nmea           = m1425_set_nmea,
};
