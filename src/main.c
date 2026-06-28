/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 * Copyright (c) 2026 Dave Goncalves
 */
#include "lbe_device.h"
#include "lbe_model.h"
#include "lbe_common.h"
#include "lbe_platform.h"
#include "cli_view.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MODEL_GENERIC (-1)

/* Friendly name for the status-report bytes the decoder understands, used by
 * --probe-op to annotate which field an opcode touched. NULL if unknown. */
static const char *status_byte_name(int off) {
	switch (off) {
	case 1:  return "status bits";
	case 6: case 7: case 8: case 9:     return "OUT1 frequency";
	case 14: case 15: case 16: case 17: return "OUT2 frequency";
	case 18: return "PLL/FLL mode";
	case 19: return "OUT1 power";
	case 20: return "OUT2 power";
	case 21: return "GNSS mask";
	case 22: return "dynModel";
	case 23: return "antenna mA";
	case 24: return "NMEA enable";
	default: return NULL;
	}
}


int main(int argc, char *argv[]) {
	struct lbe_device *dev;
	struct lbe_status status = {0};   /* zero so the unwritten report tail is defined */
	enum lbe_model model;
	int changed = 0;
	uint16_t preferred_pid = 0;
	int help_requested = 0;

	/* Banner + the "Connected to" line below are human-facing chatter -> stderr,
	 * so data modes that write machine-readable output to stdout (e.g.
	 * `--clocklog >> run.csv`) produce a clean, uncontaminated stream. */
	fprintf(stderr, "lbe-142x v1.4 27 Jun 2026 Leo Bodnar LBE-142x / LBE-Mini GPS clock source config\n");

	/* Pre-scan for --pid and --help so device-open can filter the
	 * enumeration and --help works without a device attached. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			help_requested = 1;
		} else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
			preferred_pid = (uint16_t)strtoul(argv[i + 1], NULL, 0);
		}
	}

	dev = lbe_open_device(preferred_pid);

	if (help_requested) {
		/* If a device is attached, show the help tailored to it. Else
		 * show the generic help covering every supported model. */
		lbe_print_usage(stdout, dev ? (int)lbe_get_model(dev) : MODEL_GENERIC,
		                dev ? lbe_device_ops(dev) : NULL);
		if (dev) lbe_close_device(dev);
		return 0;
	}

	if (!dev) {
		fprintf(stderr, "\n");
		lbe_print_usage(stdout, MODEL_GENERIC, NULL);
		return 1;
	}

	model = lbe_get_model(dev);

	if (argc == 1) {
		lbe_print_usage(stdout, model, lbe_device_ops(dev));
		lbe_close_device(dev);
		return 1;
	}

	/* Name the device from its capability vtable -- the single source of model
	 * identity (1421/1423/1425 each have their own ops entry). */
	const char *model_name = lbe_device_ops(dev)->name;
	fprintf(stderr, "Connected to LBE-%s\n", model_name);

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--f1") == 0 || strcmp(argv[i], "--f2") == 0 || 
			strcmp(argv[i], "--f1t") == 0 || strcmp(argv[i], "--f2t") == 0) {
			if (i + 1 < argc) {
				int out_no = (argv[i][3] == '1') ? 1 : 2;
				int temp = (argv[i][4] == 't');
				
				if (out_no == 2 && !lbe_device_ops(dev)->dual_output) {
					fprintf(stderr, "This model does not support output 2\n");
					continue;
				}

				unsigned long parsed = strtoul(argv[++i], NULL, 10);
				uint32_t new_freq = parsed > UINT32_MAX ? 0 : (uint32_t)parsed;
				uint32_t out_max = lbe_max_freq(dev, out_no);
				if (new_freq >= 1 && new_freq <= out_max) {
					if (temp) {
						if (lbe_set_frequency_temp(dev, out_no, new_freq) == 0) {
							printf("  Setting OUT%d temporary frequency: %u Hz\n", out_no, new_freq);
							changed = 1;
						}
					} else {
						if (lbe_set_frequency(dev, out_no, new_freq) == 0) {
							printf("  Setting OUT%d frequency and saving to flash: %u Hz\n", out_no, new_freq);
							changed = 1;
						}
					}
				} else {
					fprintf(stderr, "Invalid frequency: %u (range: 1-%u Hz)\n", new_freq, out_max);
				}
			}
		} else if (strcmp(argv[i], "--out") == 0) {
			if (i + 1 < argc) {
				int enable = atoi(argv[++i]);
				if (enable == 0 || enable == 1) {
					if (lbe_set_outputs_enable(dev, enable) == 0) {
						printf("  Set output(s) to %s\n", enable ? "enabled" : "disabled");
						changed = 1;
					}
				} else {
					fprintf(stderr, "Invalid output state: %d\n", enable);
				}
			}
		} else if (strcmp(argv[i], "--pll") == 0) {
			if (i + 1 < argc) {
				int fll_mode = atoi(argv[++i]);
				if (fll_mode == 0 || fll_mode == 1) {
					if (lbe_set_pll_mode(dev, fll_mode) == 0) {
						printf("  Set %s mode\n", fll_mode ? "FLL" : "PLL");
						changed = 1;
					}
				} else {
					fprintf(stderr, "Invalid PLL/FLL mode: %d\n", fll_mode);
				}
			}
		} else if (strcmp(argv[i], "--pps") == 0) {
			if (!lbe_device_ops(dev)->dual_output) {
				fprintf(stderr, "1PPS on OUT1 control is only supported on LBE-1421/1423/1425\n");
				continue;
			}
			if (i + 1 < argc) {
				int enable = atoi(argv[++i]);
				if (enable == 0 || enable == 1) {
					if (lbe_set_1pps(dev, enable) == 0) {
						printf("  Set 1PPS on OUT1 to %s\n", enable ? "enabled" : "disabled");
						changed = 1;
					}
				} else {
					fprintf(stderr, "Invalid 1PPS state: %d\n", enable);
				}
			}
		} else if (strcmp(argv[i], "--pwr1") == 0 || strcmp(argv[i], "--pwr2") == 0) {
			int out_no = argv[i][5] - '0';
			if (out_no == 2 && !lbe_device_ops(dev)->dual_output) {
				fprintf(stderr, "This model does not support output 2\n");
				continue;
			}
			if (i + 1 < argc) {
				int low_power = atoi(argv[++i]);
				if (low_power == 0 || low_power == 1) {
					if (lbe_set_power_level(dev, out_no, low_power) == 0) {
						printf("  Set OUT%d power to %s\n", out_no, low_power ? "low" : "normal");
						changed = 1;
					}
				} else {
					fprintf(stderr, "Invalid power level: %d\n", low_power);
				}
			}
		} else if (strcmp(argv[i], "--drive") == 0) {
			if (i + 1 < argc) {
				unsigned ma = (unsigned)strtoul(argv[++i], NULL, 10);
				if (lbe_set_drive_ma(dev, ma) == 0) {
					printf("  Set OUT1 drive strength to %u mA\n", ma);
					changed = 1;
				}
			}
		} else if (strcmp(argv[i], "--gnss") == 0) {
			if (i + 1 < argc) {
				unsigned long m = strtoul(argv[++i], NULL, 0);
				if (m > 0xFF) {
					fprintf(stderr, "Invalid GNSS mask: %s (expect 0x00-0xFF)\n", argv[i]);
				} else if (lbe_set_gnss(dev, (uint8_t)m) == 0) {
					printf("  Set GNSS mask to 0x%02lX\n", m);
					changed = 1;
				}
			}
		} else if (strcmp(argv[i], "--dynmodel") == 0) {
			if (i + 1 < argc) {
				const char *a = argv[++i];
				uint8_t dynmodel;
				if (lbe_dynmodel_parse(a, &dynmodel) != 0) {
					char dm_tokens[96];
					fprintf(stderr, "Invalid dynamic model: %s (%s or a u-blox value)\n",
					        a, lbe_dynmodel_token_list(dm_tokens, sizeof dm_tokens));
				} else if (lbe_set_dynmodel(dev, dynmodel) == 0) {
					printf("  Set dynamic model to %d\n", dynmodel);
					changed = 1;
				}
			}
		} else if (strcmp(argv[i], "--nmea") == 0) {
			if (i + 1 < argc) {
				int enable = atoi(argv[++i]);
				if (enable == 0 || enable == 1) {
					if (lbe_set_nmea(dev, enable) == 0) {
						printf("  Set NMEA output %s\n", enable ? "enabled" : "disabled");
						changed = 1;
					}
				} else {
					fprintf(stderr, "Invalid NMEA state: %d\n", enable);
				}
			}
		} else if (strcmp(argv[i], "--blink") == 0) {
			if (lbe_blink_leds(dev) == 0) {
				printf("  Blink LED(s)\n");
				changed = 1;
			}
		} else if (strcmp(argv[i], "--status") == 0) {
			if (lbe_get_device_status(dev, &status) != 0) {
				fprintf(stderr, "Status read failed (device busy or "
				                "transient fault -- try again)\n");
			} else {
				char serial[64];
				if (lbe_get_serial(dev, serial, sizeof serial) == 0)
					printf("  Serial: %s\n", serial);
				lbe_format_status(stdout, model, lbe_device_ops(dev), &status);
			}
		} else if (strcmp(argv[i], "--statlog") == 0) {
			/* Poll the status report ~1 Hz and print the lock state plus the
			 * raw report tail (bytes the decoder ignores, e.g. offset 21 on
			 * the 1421/1425). For studying how the tail moves through
			 * acquiring->locked and over time (temperature/voltage?). */
			printf("time      raw  GPS PLL ANT | bytes[18..40]   (Ctrl-C to stop)\n");
			for (;;) {
				if (lbe_get_device_status(dev, &status) != 0) {
					/* The transport already retries genuinely transient
					 * control-pipe errors (EPIPE/EAGAIN/EBUSY/EINTR), so a
					 * failure that surfaces here means the device went away
					 * (USB unplug / re-enumerate). Stop with one message
					 * instead of spinning and spamming the per-poll error
					 * forever. */
					fprintf(stderr, "--statlog: status read failed "
					        "(device disconnected?); stopping.\n");
					break;
				}
				time_t now = time(NULL);
				struct tm *lt = localtime(&now);
				char ts[16];
				strftime(ts, sizeof ts, "%H:%M:%S", lt);
				printf("%s  0x%02X  %d   %d   %d  |", ts, status.raw_status,
				       !!(status.raw_status & LBE_GPS_LOCK_BIT),
				       status.pll_locked, status.antenna_ok);
				for (int b = 18; b <= 40; b++) printf(" %02X", status.raw[b]);
				printf("\n");
				fflush(stdout);
				lbe_sleep_ms(1000);
			}
		} else if (strcmp(argv[i], "--probe-op") == 0) {
			/* RE helper: send an arbitrary opcode (+ optional payload bytes)
			 * and report which status-report bytes it changed. */
			if (!lbe_device_ops(dev)->dual_output) {
				fprintf(stderr, "--probe-op needs the 1421-family status report\n");
				continue;
			}
			if (i + 1 >= argc) {
				fprintf(stderr, "usage: --probe-op <0xNN> [b1 b2 ...]\n");
				continue;
			}
			uint8_t op = (uint8_t)strtoul(argv[++i], NULL, 0);
			uint8_t payload[59];
			size_t pn = 0;
			while (i + 1 < argc && pn < sizeof payload) {
				char *end;
				unsigned long v = strtoul(argv[i + 1], &end, 0);
				if (argv[i + 1][0] == '\0' || *end != '\0' || v > 0xFF) break;
				payload[pn++] = (uint8_t)v;
				i++;
			}
			struct lbe_status before = {0}, after = {0};
			if (lbe_get_device_status(dev, &before) != 0) {
				fprintf(stderr, "  baseline status read failed\n");
				continue;
			}
			printf("Probing opcode 0x%02X", op);
			for (size_t k = 0; k < pn; k++) printf(" %02X", payload[k]);
			printf(" ...\n");
			if (lbe_send_raw(dev, op, payload, pn) != 0) {
				fprintf(stderr, "  send failed\n");
				continue;
			}
			lbe_sleep_ms(50);
			if (lbe_get_device_status(dev, &after) != 0) {
				fprintf(stderr, "  follow-up status read failed\n");
				continue;
			}
			int any = 0;
			for (int b = 0; b < 60; b++) {
				if (before.raw[b] == after.raw[b]) continue;
				const char *nm = status_byte_name(b);
				if (nm)
					printf("  byte %2d: 0x%02X -> 0x%02X  (%s)\n",
					       b, before.raw[b], after.raw[b], nm);
				else
					printf("  byte %2d: 0x%02X -> 0x%02X\n",
					       b, before.raw[b], after.raw[b]);
				any = 1;
			}
			if (!any) printf("  no status change\n");
			changed = 1;
		} else if (strcmp(argv[i], "--monitor") == 0) {
			/* Let the shared monitor impl render the real model in its
			 * title (1421/1423/1425 share one monitor function). */
			lbe_setenv("LBE_MODEL_NAME", model_name);
			lbe_monitor(dev);
			changed = 1;
		} else if (strcmp(argv[i], "--gps-info") == 0) {
			lbe_gps_info(dev);
			changed = 1;
		} else if (strcmp(argv[i], "--diag") == 0) {
			lbe_diag(dev);
			changed = 1;
		} else if (strcmp(argv[i], "--clocklog") == 0) {
			/* --clocklog [seconds]: CSV NAV-CLOCK time series. Optional
			 * positive duration; default runs until Ctrl-C. */
			int seconds = 0;
			if (i + 1 < argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
				seconds = atoi(argv[++i]);
			lbe_clocklog(dev, seconds);
			changed = 1;
		} else if (strcmp(argv[i], "--rawdump") == 0) {
			/* Hidden RE helper: --rawdump [ep] [ms]. Defaults ep=0x81,
			 * duration=2000. Reads interrupt-IN and hex-dumps frames. */
			uint8_t ep = 0x81;
			int ms = 2000;
			if (i + 1 < argc && argv[i+1][0] == '0') {
				ep = (uint8_t)strtoul(argv[++i], NULL, 0);
				if (i + 1 < argc) ms = atoi(argv[++i]);
			}
			lbe_rawdump(dev, ep, ms);
			changed = 1;
		} else if (strcmp(argv[i], "--port") == 0) {
			/* Propagate to the monitor impl via env var so we don't
			 * have to plumb a string through lbe_model_ops. */
			if (i + 1 < argc) {
				lbe_setenv("LBE_PORT", argv[++i]);
			}
		} else if (strcmp(argv[i], "--pid") == 0) {
			i++;  /* consumed in the pre-scan above */
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			lbe_print_usage(stdout, model, lbe_device_ops(dev));
		}
	}

	if (!changed) {
		printf("No changes made\n");
	}

	lbe_close_device(dev);
	return 0;
}