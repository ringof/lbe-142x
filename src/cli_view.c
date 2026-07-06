/*
 * SPDX-License-Identifier: MIT
 */
/* CLI presentation: --help and --status rendering, plus the dynamic-model
 * table they share with the --dynmodel parser. Pure functions of (model, ops,
 * status) -> text on a caller-supplied stream; no device I/O, so tests/test_cli.c
 * can drive every model's help/status gating with no hardware (issue #8). */
#include "cli_view.h"
#include "lbe_model.h"
#include "lbe_common.h"

#include <string.h>
#include <stdlib.h>

/* Single source of truth for the u-blox CFG-NAV5 dynamic platform model
 * (value <-> name). `token` is the lowercase CLI keyword (NULL = settable by
 * numeric value only, matching the vendor UI, which exposes no name for it);
 * `display` is the label shown by --status. */
static const struct { uint8_t value; const char *token; const char *display; } DYNMODEL[] = {
	{0, "portable",   "Portable"},
	{2, "stationary", "Stationary"},
	{3, "pedestrian", "Pedestrian"},
	{4, "automotive", "Automotive"},
	{5, "sea",        "Sea"},
	{6, NULL,         "Airborne<1g"},
	{7, NULL,         "Airborne<2g"},
	{8, "airborne",   "Airborne<4g"},
};

const char *lbe_dynmodel_token_list(char *buf, size_t n) {
	size_t off = 0;
	for (size_t i = 0; i < sizeof DYNMODEL / sizeof DYNMODEL[0]; i++) {
		if (!DYNMODEL[i].token) continue;
		int w = snprintf(buf + off, n - off, "%s%s",
		                 off ? "|" : "", DYNMODEL[i].token);
		if (w < 0 || (size_t)w >= n - off) break;
		off += (size_t)w;
	}
	return buf;
}

int lbe_dynmodel_parse(const char *arg, uint8_t *out) {
	for (size_t k = 0; k < sizeof DYNMODEL / sizeof DYNMODEL[0]; k++)
		if (DYNMODEL[k].token && strcmp(arg, DYNMODEL[k].token) == 0) {
			*out = DYNMODEL[k].value;
			return 0;
		}
	char *end;
	unsigned long v = strtoul(arg, &end, 0);   /* not a keyword -- raw value */
	if (*end == '\0' && v <= 0xFF) { *out = (uint8_t)v; return 0; }
	return -1;
}

void lbe_print_usage(FILE *out, int model, const struct lbe_model_ops *ops) {
	int generic = (ops == NULL);
	int is_mini = !generic && (model == LBE_MINI);
	int dual    = generic || ops->dual_output;
	unsigned long mf = generic ? 0 : ops->max_freq_out1;

	fprintf(out, "Usage: lbe-142x [OPTIONS]\n");
	fprintf(out, "Options:\n");
	fprintf(out, "  --help                 Show this help\n");
	fprintf(out, "  --pid <0xNNNN>         Select a specific LBE device when more than one is attached\n");
	fprintf(out, "                         (0x2443=1420, 0x2444=1421, 0x226f=1423, 0x2269=1425, 0x2211=Mini)\n");

	if (generic)
		fprintf(out, "  --f1 <Hz>              Set OUT1 frequency, save to flash (1420 <=%lu, 1421/1423 <=%lu, 1425 OUT1 <=%lu, Mini <=%lu)\n",
		        LBE_1420_MAX_FREQ, LBE_1421_MAX_FREQ, LBE_1425_OUT1_MAX_FREQ, LBE_MINI_MAX_FREQ);
	else
		fprintf(out, "  --f1 <Hz>              Set OUT1 frequency (1-%lu Hz), save to flash\n", mf);

	if (generic || !is_mini)
		fprintf(out, "  --f1t <Hz>             Set OUT1 temporary frequency%s\n",
		        generic ? " (not supported on Mini)" : "");

	if (dual) {
		fprintf(out, "  --f2 <Hz>              Set OUT2 frequency, save to flash%s\n",
		        generic ? " (LBE-1421/1423/1425)" : "");
		fprintf(out, "  --f2t <Hz>             Set OUT2 temporary frequency%s\n",
		        generic ? " (LBE-1421/1423/1425)" : "");
	}

	fprintf(out, "  --out <0|1>            Enable or disable outputs\n");

	if (generic || !is_mini)
		fprintf(out, "  --pll <0|1>            Set PLL(0) or FLL(1) mode%s\n",
		        generic ? " (not supported on Mini)" : "");

	if (dual)
		fprintf(out, "  --pps <0|1>            Enable or disable 1PPS on OUT1%s\n",
		        generic ? " (LBE-1421/1423/1425)" : "");

	fprintf(out, "  --pwr1 <0|1>           Set OUT1 power level: normal(0) or low(1)\n");

	if (dual)
		fprintf(out, "  --pwr2 <0|1>           Set OUT2 power level: normal(0) or low(1)%s\n",
		        generic ? " (LBE-1421/1423/1425)" : "");

	if (generic || ops->set_drive_ma)
		fprintf(out, "  --drive <8|16|24|32>   Set OUT1 drive strength in mA%s\n",
		        generic ? " (Mini only)" : "");

	if (generic || ops->set_gnss) {
		fprintf(out, "  --gnss <0xNN>          Set GNSS constellation bitmask"
		        " (GPS=0x01 SBAS=0x02 Gal=0x04 BeiDou=0x08 QZSS=0x20 GLO=0x40 NavIC=0x80)%s\n",
		        generic ? " (LBE-1420/1425)" : "");
		char dm_tokens[96];
		fprintf(out, "  --dynmodel <model>     Set u-blox dynamic model (%s)%s\n",
		        lbe_dynmodel_token_list(dm_tokens, sizeof dm_tokens),
		        generic ? " (LBE-1420/1425)" : "");
	}
	if (generic || (ops && ops->set_nmea))
		fprintf(out, "  --nmea <0|1>           Enable or disable NMEA output%s\n",
		        generic ? " (LBE-1425 only)" : "");
	if (generic || (ops && ops->diag)) {
		fprintf(out, "  --diag                 Live UBX diagnostics (CNR histogram + clock"
		        " disciplining)%s\n", generic ? " (LBE-1420/1425)" : "");
		fprintf(out, "  --clocklog [seconds]   CSV NAV-CLOCK time series for plotting"
		        " (Ctrl-C, or run N s)%s\n", generic ? " (LBE-1420/1425)" : "");
	}

	fprintf(out, "  --blink                Blink output LED(s) for 3 seconds\n");
	fprintf(out, "  --status               Display current device status\n");

	if (dual)
		fprintf(out, "  --statlog              Poll status ~1 Hz, log lock state + raw report tail%s\n",
		        generic ? " (LBE-142x)" : "");

	fprintf(out, "  --probe-op <0xNN> [b..] Send a raw opcode + bytes, show status changes (advanced)\n");

	if (generic || ops->monitor)
		fprintf(out, "  --monitor              Live GPS display (UTC, lat/lon, altitude, CNR bars)%s\n",
		        generic ? " (Mini: UBX; 1420/1421/1423/1425: NMEA via CDC)" : "");

	if (generic || (ops->monitor && !is_mini))
		fprintf(out, "  --port <name>          CDC port for --monitor (e.g. COM12 or /dev/ttyACM0)%s\n",
		        generic ? " (LBE-1420/1421/1423/1425)" : "");

	if (generic || ops->gps_info)
		fprintf(out, "  --gps-info             Print u-blox GPS module version + antenna status%s\n",
		        generic ? " (Mini / LBE-1420 / LBE-1425)" : "");
}

void lbe_format_status(FILE *out, int model, const struct lbe_model_ops *ops,
                       const struct lbe_status *s) {
	fprintf(out, "Device Status (0x%02X):\n", s->raw_status);
	fprintf(out, "  GPS Lock: %s\n", (s->raw_status & LBE_GPS_LOCK_BIT) ? "Yes" : "No");
	fprintf(out, "  PLL Lock: %s\n", s->pll_locked ? "Yes" : "No");
	/* Antenna status is not decodable on the Mini feature report -- the vendor
	 * UI does not expose it either. */
	if (model != LBE_MINI) {
		if (!s->antenna_ok) {
			fprintf(out, "  Antenna: Short Circuit\n");
		} else if (ops && ops->has_antenna_current) {
			/* Bias current lets us tell "no antenna" (0 mA) from a healthy one;
			 * the short-circuit bit alone can't. */
			if (s->antenna_current_ma == 0)
				fprintf(out, "  Antenna: Not connected (0 mA)\n");
			else
				fprintf(out, "  Antenna: OK (%u mA)\n", s->antenna_current_ma);
		} else {
			fprintf(out, "  Antenna: OK\n");
		}
	}
	fprintf(out, "  Output(s) Enabled: %s\n", s->outputs_enabled ? "Yes" : "No");
	fprintf(out, "  OUT1 Frequency: %u Hz\n", s->frequency1);
	if (model == LBE_MINI) {
		fprintf(out, "  OUT1 Drive Strength: %umA\n", s->out1_drive_ma);
		fprintf(out, "  Signal loss count: %u\n", s->signal_loss_count);
	} else {
		fprintf(out, "  OUT1 Power Level: %s\n", s->out1_power_low ? "Low" : "Normal");
	}

	if (ops && ops->dual_output) {
		fprintf(out, "  OUT2 Frequency: %u Hz\n", s->frequency2);
		fprintf(out, "  OUT2 Power Level: %s\n", s->out2_power_low ? "Low" : "Normal");
		fprintf(out, "  1PPS on OUT1: %s\n", s->pps_enabled ? "Enabled" : "Disabled");
	}
	/* Mini has no FLL/PLL mode toggle. */
	if (model != LBE_MINI) {
		fprintf(out, "  Mode: %s\n", s->fll_enabled ? "FLL" : "PLL");
	}
	/* 1420/1425 echo the GNSS mask and dynamic model in their status reports
	 * (at model-specific byte offsets, normalized into struct fields). */
	if (ops && ops->set_gnss) {
		static const struct { uint8_t bit; const char *name; } gn[] = {
			{LBE_1425_GNSS_GPS, "GPS"}, {LBE_1425_GNSS_SBAS, "SBAS"},
			{LBE_1425_GNSS_GALILEO, "Galileo"}, {LBE_1425_GNSS_BEIDOU, "BeiDou"},
			{LBE_1425_GNSS_IMES, "IMES"}, {LBE_1425_GNSS_QZSS, "QZSS"},
			{LBE_1425_GNSS_GLONASS, "GLONASS"}, {LBE_1425_GNSS_NAVIC, "NavIC"},
		};
		uint8_t mask = s->gnss_mask;
		fprintf(out, "  GNSS: 0x%02X (", mask);
		int first = 1;
		for (size_t g = 0; g < sizeof gn / sizeof gn[0]; g++)
			if (mask & gn[g].bit) {
				fprintf(out, "%s%s", first ? "" : " ", gn[g].name);
				first = 0;
			}
		fprintf(out, "%s)\n", first ? "none" : "");
		const char *dm = "?";
		for (size_t k = 0; k < sizeof DYNMODEL / sizeof DYNMODEL[0]; k++)
			if (DYNMODEL[k].value == s->dynmodel) {
				dm = DYNMODEL[k].display;
				break;
			}
		fprintf(out, "  Dynamic model: %s (%u)\n", dm, s->dynmodel);
	}
	if (ops && ops->set_nmea)
		fprintf(out, "  NMEA output: %s\n", s->raw[24] ? "Enabled" : "Disabled");
}
