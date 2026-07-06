/*
 * SPDX-License-Identifier: MIT
 */
#include "lbe_common.h"
#include "lbe_model.h"
#include "lbe_transport.h"
#include "lbe_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The LBE-1420 (bcdDevice 1.08) uses the 1421 wire format: opcode in payload
 * byte 0 doubles as the HID Report ID, frequency LE32 at byte offset 5, status
 * read via Report ID 0x4B.  Confirmed via vendor-tool capture (Rung 2).
 *
 * Differences from the 1421/1425:
 *  - Single output (no freq2, no OUT2 power, no 1PPS).
 *  - Status bit 4 = output-enable (1421/1425 use bits 5+6).
 *  - GNSS mask at status byte 10 (not 21), dynModel at byte 11 (not 22),
 *    antenna current at byte 12 (not 23).
 *  - Power level is write-only (not echoed in status).
 *  - GNSS opcode 0x07 (1425 uses 0x03), dynModel opcode 0x09 (1425 uses 0x04).
 *  - EP 0x83 UBX diagnostic stream is one-shot (re-poll at ~1 Hz). */
#define LBE_STATUS_REPORT_ID 0x4B
#define LBE_1420_OUT_EN_BIT  (1 << 4)

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

static int m1420_get_status(struct lbe_transport *t, struct lbe_status *s) {
	uint8_t buf[LBE_REPORT_SIZE] = {0};

	/* Retry-on-zero-freq: same transient-zero guard as model_1421.c. */
	for (int attempt = 0; attempt < 2; attempt++) {
		memset(buf, 0, sizeof buf);
		if (lbe_transport_feat_get(t, LBE_STATUS_REPORT_ID, buf) < 0) return -1;
		uint32_t f1 = buf[6] | (buf[7] << 8) | (buf[8] << 16) | (buf[9] << 24);
		if (f1 != 0) break;
		if (attempt == 0) lbe_sleep_ms(20);
		else return -1;
	}

	memcpy(s->raw, buf, LBE_REPORT_SIZE);
	s->raw_status     = buf[1];
	s->frequency1     = buf[6] | (buf[7] << 8) | (buf[8] << 16) | (buf[9] << 24);
	s->frequency2     = 0;
	s->gnss_mask      = buf[10];   /* GNSS constellation mask */
	s->dynmodel       = buf[11];   /* u-blox dynamic model */
	s->antenna_current_ma = buf[12];   /* antenna bias current (mA) */
	s->fll_enabled    = buf[18] != 0;
	s->out1_power_low = 0;   /* write-only on the 1420 (not echoed in status) */
	s->out2_power_low = 0;
	s->out1_drive_ma  = 0;
	s->pll_locked     = (s->raw_status & LBE_PLL_LOCK_BIT) != 0;
	s->antenna_ok     = (s->raw_status & LBE_ANT_OK_BIT) != 0;
	s->pps_enabled    = 0;
	s->outputs_enabled = (s->raw_status & LBE_1420_OUT_EN_BIT) != 0;
	return 0;
}

static int m1420_set_frequency(struct lbe_transport *t, int output, uint32_t hz) {
	if (output != 1) {
		fprintf(stderr, "LBE-1420 only supports output 1\n");
		return -1;
	}
	uint8_t args[4] = {
		(uint8_t)(hz),
		(uint8_t)(hz >> 8),
		(uint8_t)(hz >> 16),
		(uint8_t)(hz >> 24),
	};
	return send_cmd(t, LBE_1421_SET_F1, args, 5, sizeof args);
}

static int m1420_set_frequency_temp(struct lbe_transport *t, int output, uint32_t hz) {
	if (output != 1) {
		fprintf(stderr, "LBE-1420 only supports output 1\n");
		return -1;
	}
	uint8_t args[4] = {
		(uint8_t)(hz),
		(uint8_t)(hz >> 8),
		(uint8_t)(hz >> 16),
		(uint8_t)(hz >> 24),
	};
	return send_cmd(t, LBE_1421_SET_F1_TEMP, args, 5, sizeof args);
}

static int m1420_set_outputs_enable(struct lbe_transport *t, int enable) {
	uint8_t arg = enable ? 0xFF : 0x00;   /* vendor sends 0xFF for on */
	return send_cmd(t, LBE_142X_EN_OUT, &arg, 1, 1);
}

static int m1420_blink_leds(struct lbe_transport *t) {
	return send_cmd(t, LBE_142X_BLINK_OUT, NULL, 1, 0);
}

static int m1420_set_pll_mode(struct lbe_transport *t, int fll_mode) {
	uint8_t arg = fll_mode ? 0x01 : 0x00;
	return send_cmd(t, LBE_142X_SET_PLL, &arg, 1, 1);
}

static int m1420_set_power_level(struct lbe_transport *t, int output, int low) {
	if (output != 1) {
		fprintf(stderr, "LBE-1420 only supports output 1\n");
		return -1;
	}
	uint8_t arg = low ? 0x01 : 0x00;
	return send_cmd(t, LBE_1421_SET_PWR1, &arg, 1, 1);
}

static int m1420_set_gnss(struct lbe_transport *t, uint8_t mask) {
	return send_cmd(t, LBE_1420_SET_GNSS, &mask, 1, 1);
}

static int m1420_set_dynmodel(struct lbe_transport *t, uint8_t model) {
	return send_cmd(t, LBE_1420_SET_DYNMODEL, &model, 1, 1);
}

const struct lbe_model_ops lbe_ops_1420 = {
	.name               = "1420",
	.max_freq_out1      = LBE_1420_MAX_FREQ,
	.max_freq_out2      = LBE_1420_MAX_FREQ,
	.init               = NULL,
	.get_status         = m1420_get_status,
	.set_frequency      = m1420_set_frequency,
	.set_frequency_temp = m1420_set_frequency_temp,
	.set_outputs_enable = m1420_set_outputs_enable,
	.blink_leds         = m1420_blink_leds,
	.set_pll_mode       = m1420_set_pll_mode,
	.set_1pps           = NULL,
	.set_power_level    = m1420_set_power_level,
	.monitor            = lbe_shared_monitor,
	.gps_info           = lbe_shared_gps_info,
	.set_gnss           = m1420_set_gnss,
	.set_dynmodel       = m1420_set_dynmodel,
	.diag               = lbe_shared_diag,
	.clocklog           = lbe_shared_clocklog,
	.has_antenna_current = 1,
};
