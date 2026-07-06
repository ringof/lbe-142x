/*
 * SPDX-License-Identifier: MIT
 */
#ifndef LBE_MODEL_H
#define LBE_MODEL_H

#include <stdint.h>
#include "lbe_device.h"
#include "lbe_transport.h"

/* Each model implements this vtable and exports it as a const
 * symbol. lbe_device.c maps the USB PID to an ops pointer at open
 * time. Unsupported operations for a model (e.g. 1PPS on 1420) may
 * be NULL -- lbe_device.c then returns -1 and an error message
 * without dispatching. */
struct lbe_model_ops {
	const char *name;
	/* Max settable frequency per output in Hz. Symmetric models set both
	 * fields equal; the 1425 caps OUT1 (its 1PPS output) below OUT2. Read
	 * via lbe_max_freq(). */
	uint32_t max_freq_out1;
	uint32_t max_freq_out2;

	/* Optional device init called once after transport open. */
	int (*init)(struct lbe_transport *t);

	int (*get_status)(struct lbe_transport *t, struct lbe_status *s);
	int (*set_frequency)(struct lbe_transport *t, int output, uint32_t hz);
	int (*set_frequency_temp)(struct lbe_transport *t, int output, uint32_t hz);
	int (*set_outputs_enable)(struct lbe_transport *t, int enable);
	int (*blink_leds)(struct lbe_transport *t);
	int (*set_pll_mode)(struct lbe_transport *t, int fll_mode);
	int (*set_1pps)(struct lbe_transport *t, int enable);
	int (*set_power_level)(struct lbe_transport *t, int output, int low);

	/* Mini only: set OUT1 drive strength to one of 8/16/24/32 mA. NULL
	 * on 1420/1421 which use the bool --pwr1/--pwr2 API instead. */
	int (*set_drive_ma)(struct lbe_transport *t, unsigned ma);

	/* Interactive GPS monitor (Ctrl-C to exit). Mini parses UBX from HID;
	 * 1420/1421/1423/1425 parse NMEA from the CDC port. NULL means "unsupported". */
	int (*monitor)(struct lbe_transport *t);

	/* One-shot UBX info poll (MON-VER, CFG-GNSS, CFG-TP5). Mini / 1420
	 * / 1425; NULL elsewhere. Prints to stdout, returns 0 on success. */
	int (*gps_info)(struct lbe_transport *t);

	/* 1420/1425 (NULL elsewhere). GNSS constellation enable bitmask,
	 * u-blox dynamic platform model, and NMEA-output enable (1425 only). */
	int (*set_gnss)(struct lbe_transport *t, uint8_t mask);
	int (*set_dynmodel)(struct lbe_transport *t, uint8_t model);
	int (*set_nmea)(struct lbe_transport *t, int enable);

	/* 1420/1425: live UBX diagnostics monitor (NAV-PVT/SAT/CLOCK from
	 * the EP 0x83 stream). NULL elsewhere. Loops until killed. */
	int (*diag)(struct lbe_transport *t);

	/* 1420/1425: CSV time-series log of NAV-CLOCK timing telemetry from
	 * the EP 0x83 stream (one row per ~1 Hz solution). `seconds` bounds the
	 * run; <=0 means until killed. NULL elsewhere. */
	int (*clocklog)(struct lbe_transport *t, int seconds);

	/* --- capability traits ------------------------------------------------
	 * Capabilities that don't map cleanly onto a NULL-able op pointer, so the
	 * CLI can gate help/status off the vtable instead of PID/enum literals. */

	/* Has a second output (OUT2). Set on the dual-output 1421/1423/1425; 0 on
	 * single-output 1420/Mini. Gates --f2/--f2t/--pwr2/--pps/--statlog/
	 * --probe-op/--port help and the OUT2 / 1PPS status rows. */
	int dual_output;

	/* Reports antenna bias current (mA) so "no antenna" (0 mA) is
	 * distinguishable from a healthy one -- the short-circuit bit alone
	 * can't. Set on the 1420 (byte 12) and 1425 (byte 23); 0 elsewhere. */
	int has_antenna_current;
};

/* Shared ops used by both the 1420 and 1425 vtables (defined in model_1421.c).
 * The monitor function reads NMEA from the CDC port; diag/clocklog/gps_info
 * use the EP 0x83 UBX stream. */
int lbe_shared_monitor(struct lbe_transport *t);
int lbe_shared_diag(struct lbe_transport *t);
int lbe_shared_clocklog(struct lbe_transport *t, int seconds);
int lbe_shared_gps_info(struct lbe_transport *t);

extern const struct lbe_model_ops lbe_ops_1420;
extern const struct lbe_model_ops lbe_ops_1421;
extern const struct lbe_model_ops lbe_ops_1423;
extern const struct lbe_model_ops lbe_ops_1425;
extern const struct lbe_model_ops lbe_ops_mini;

#endif
