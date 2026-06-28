/*
 * SPDX-License-Identifier: MIT
 */
/* Hardware-free tests for the CLI presentation layer (src/cli_view.c): prove
 * that --help and --status gating is correct for every model, driven purely by
 * the capability vtable -- no device, no transport (issue #8). Output is
 * captured via tmpfile() (portable, incl. MSVC -- no open_memstream/fmemopen). */
#include "cli_view.h"
#include "lbe_model.h"
#include "lbe_device.h"
#include "lbe_common.h"
#include "test_util.h"

#include <stdio.h>
#include <string.h>

#define MODEL_GENERIC (-1)

/* Dummy ops -- only the address (non-NULL) matters; the render functions gate
 * on `!= NULL` and never call them. Signatures match the vtable field types. */
static int d_t(struct lbe_transport *t) { (void)t; return 0; }
static int d_tu(struct lbe_transport *t, uint8_t u) { (void)t; (void)u; return 0; }
static int d_ti(struct lbe_transport *t, int i) { (void)t; (void)i; return 0; }
static int d_tum(struct lbe_transport *t, unsigned m) { (void)t; (void)m; return 0; }

/* tmpfile() is flagged C4996 (deprecated) by MSVC /WX; use its non-deprecated
 * tmpfile_s() there, plain tmpfile() elsewhere. */
static FILE *open_tmp(void) {
#if defined(_MSC_VER)
	FILE *f = NULL;
	return tmpfile_s(&f) == 0 ? f : NULL;
#else
	return tmpfile();
#endif
}

static void cap_usage(char *buf, size_t cap, int model,
                      const struct lbe_model_ops *ops) {
	FILE *f = open_tmp();
	if (!f) { buf[0] = '\0'; return; }
	lbe_print_usage(f, model, ops);
	long n = ftell(f);
	if (n < 0) n = 0;
	if ((size_t)n >= cap) n = (long)cap - 1;
	rewind(f);
	size_t got = fread(buf, 1, (size_t)n, f);
	buf[got] = '\0';
	fclose(f);
}

static void cap_status(char *buf, size_t cap, int model,
                       const struct lbe_model_ops *ops,
                       const struct lbe_status *s) {
	FILE *f = open_tmp();
	if (!f) { buf[0] = '\0'; return; }
	lbe_format_status(f, model, ops, s);
	long n = ftell(f);
	if (n < 0) n = 0;
	if ((size_t)n >= cap) n = (long)cap - 1;
	rewind(f);
	size_t got = fread(buf, 1, (size_t)n, f);
	buf[got] = '\0';
	fclose(f);
}

static int has(const char *hay, const char *needle) {
	return strstr(hay, needle) != NULL;
}

/* Per-model fixtures mirroring the real capability sets (cf. the lbe_ops_*
 * tables). Only the fields the render functions consult are set. */
static struct lbe_model_ops fix_1420(void) {
	struct lbe_model_ops o; memset(&o, 0, sizeof o);
	o.name = "1420";
	o.max_freq_out1 = LBE_1420_MAX_FREQ; o.max_freq_out2 = LBE_1420_MAX_FREQ;
	return o;   /* single output; no monitor/gnss/drive/gps_info */
}
static struct lbe_model_ops fix_1421(void) {
	struct lbe_model_ops o; memset(&o, 0, sizeof o);
	o.name = "1421 dual output";
	o.max_freq_out1 = LBE_1421_MAX_FREQ; o.max_freq_out2 = LBE_1421_MAX_FREQ;
	o.dual_output = 1; o.monitor = d_t;
	return o;
}
static struct lbe_model_ops fix_1425(void) {
	struct lbe_model_ops o; memset(&o, 0, sizeof o);
	o.name = "1425 dual output";
	o.max_freq_out1 = LBE_1425_OUT1_MAX_FREQ; o.max_freq_out2 = LBE_1425_OUT2_MAX_FREQ;
	o.dual_output = 1; o.has_antenna_current = 1; o.monitor = d_t;
	o.set_gnss = d_tu; o.set_dynmodel = d_tu; o.set_nmea = d_ti;
	o.diag = d_t; o.clocklog = d_ti; o.gps_info = d_t;
	return o;
}
static struct lbe_model_ops fix_mini(void) {
	struct lbe_model_ops o; memset(&o, 0, sizeof o);
	o.name = "Mini";
	o.max_freq_out1 = LBE_MINI_MAX_FREQ; o.max_freq_out2 = LBE_MINI_MAX_FREQ;
	o.set_drive_ma = d_tum; o.monitor = d_t; o.gps_info = d_t;
	return o;
}

static void want_cap(char *out, size_t n, unsigned long hz) {
	snprintf(out, n, "1-%lu Hz", hz);   /* the tailored --f1 cap substring */
}

void run_cli_tests(void) {
	printf("cli:\n");
	char b[4096], cap[64];
	struct lbe_model_ops o;
	struct lbe_status s;

	/* generic help: the full option set is shown */
	cap_usage(b, sizeof b, MODEL_GENERIC, NULL);
	CHECK(has(b, "--f1 ") && has(b, "--f2 ") && has(b, "--gnss") && has(b, "--diag")
	   && has(b, "--clocklog") && has(b, "--drive") && has(b, "--gps-info")
	   && has(b, "--monitor") && has(b, "--statlog") && has(b, "--pps"));

	/* 1425: dual-output + full GNSS/diag/clocklog + gps-info; OUT1 cap 800 MHz */
	o = fix_1425();
	cap_usage(b, sizeof b, LBE_1421_DUALOUT, &o);
	CHECK(has(b, "--f2 ") && has(b, "--pps") && has(b, "--statlog") && has(b, "--port"));
	CHECK(has(b, "--gnss") && has(b, "--dynmodel") && has(b, "--nmea")
	   && has(b, "--diag") && has(b, "--clocklog") && has(b, "--gps-info"));
	CHECK(has(b, "--monitor"));
	CHECK(!has(b, "--drive"));
	want_cap(cap, sizeof cap, (unsigned long)LBE_1425_OUT1_MAX_FREQ);
	CHECK(has(b, cap));

	/* 1421: dual-output, no GNSS family, no drive; OUT1 cap 1.4 GHz */
	o = fix_1421();
	cap_usage(b, sizeof b, LBE_1421_DUALOUT, &o);
	CHECK(has(b, "--f2 ") && has(b, "--pps") && has(b, "--statlog") && has(b, "--monitor"));
	CHECK(!has(b, "--gnss") && !has(b, "--diag") && !has(b, "--clocklog")
	   && !has(b, "--gps-info") && !has(b, "--drive"));
	want_cap(cap, sizeof cap, (unsigned long)LBE_1421_MAX_FREQ);
	CHECK(has(b, cap));

	/* 1420: single output; no f2/pps/statlog/gnss/monitor/drive/gps-info */
	o = fix_1420();
	cap_usage(b, sizeof b, LBE_1420, &o);
	CHECK(!has(b, "--f2 ") && !has(b, "--pps") && !has(b, "--statlog")
	   && !has(b, "--gnss") && !has(b, "--monitor") && !has(b, "--drive")
	   && !has(b, "--gps-info"));
	CHECK(has(b, "--f1t") && has(b, "--pll"));   /* non-Mini shows these */
	want_cap(cap, sizeof cap, (unsigned long)LBE_1420_MAX_FREQ);
	CHECK(has(b, cap));

	/* Mini: drive + gps-info + monitor; no f2/pps/gnss/statlog; no pll/f1t */
	o = fix_mini();
	cap_usage(b, sizeof b, LBE_MINI, &o);
	CHECK(has(b, "--drive") && has(b, "--gps-info") && has(b, "--monitor"));
	CHECK(!has(b, "--f2 ") && !has(b, "--pps") && !has(b, "--gnss") && !has(b, "--statlog"));
	CHECK(!has(b, "--pll") && !has(b, "--f1t"));   /* "not supported on Mini" */
	want_cap(cap, sizeof cap, (unsigned long)LBE_MINI_MAX_FREQ);
	CHECK(has(b, cap));

	/* status, 1425: antenna mA + OUT2 + 1PPS + GNSS echo + Mode */
	o = fix_1425();
	memset(&s, 0, sizeof s);
	s.antenna_ok = 1; s.antenna_current_ma = 5; s.outputs_enabled = 1;
	s.raw[21] = LBE_1425_GNSS_GPS; s.raw[22] = 2 /*Stationary*/; s.raw[24] = 1;
	cap_status(b, sizeof b, LBE_1421_DUALOUT, &o, &s);
	CHECK(has(b, "Antenna: OK (5 mA)"));
	CHECK(has(b, "OUT2 Frequency") && has(b, "1PPS on OUT1") && has(b, "Mode:"));
	CHECK(has(b, "GNSS: 0x01 (GPS)") && has(b, "Dynamic model: Stationary (2)")
	   && has(b, "NMEA output: Enabled"));
	s.antenna_current_ma = 0;   /* 0 mA -> "Not connected" on the 1425 */
	cap_status(b, sizeof b, LBE_1421_DUALOUT, &o, &s);
	CHECK(has(b, "Antenna: Not connected (0 mA)"));

	/* status, 1421: plain "Antenna: OK", OUT2 rows, no mA, no GNSS echo */
	o = fix_1421();
	memset(&s, 0, sizeof s); s.antenna_ok = 1;
	cap_status(b, sizeof b, LBE_1421_DUALOUT, &o, &s);
	CHECK(has(b, "Antenna: OK\n") && !has(b, "mA)"));
	CHECK(has(b, "OUT2 Frequency") && !has(b, "GNSS:"));

	/* status, Mini: drive + signal-loss; no antenna/OUT2/Mode rows */
	o = fix_mini();
	memset(&s, 0, sizeof s); s.out1_drive_ma = 24; s.signal_loss_count = 3;
	cap_status(b, sizeof b, LBE_MINI, &o, &s);
	CHECK(has(b, "OUT1 Drive Strength: 24mA") && has(b, "Signal loss count: 3"));
	CHECK(!has(b, "Antenna:") && !has(b, "OUT2") && !has(b, "Mode:"));

	/* status: antenna short-circuit branch (non-Mini, !antenna_ok) */
	o = fix_1421();
	memset(&s, 0, sizeof s); s.antenna_ok = 0;
	cap_status(b, sizeof b, LBE_1421_DUALOUT, &o, &s);
	CHECK(has(b, "Antenna: Short Circuit"));

	/* dynmodel parse: keyword, raw decimal/hex, and rejects */
	uint8_t dm;
	CHECK(lbe_dynmodel_parse("stationary", &dm) == 0 && dm == 2);
	CHECK(lbe_dynmodel_parse("8", &dm) == 0 && dm == 8);
	CHECK(lbe_dynmodel_parse("0x08", &dm) == 0 && dm == 8);
	CHECK(lbe_dynmodel_parse("bogus", &dm) != 0);
	CHECK(lbe_dynmodel_parse("300", &dm) != 0);   /* > 0xFF */
	char tl[96];
	lbe_dynmodel_token_list(tl, sizeof tl);
	CHECK(has(tl, "portable") && has(tl, "stationary") && has(tl, "airborne"));
}
