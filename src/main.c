/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#include "lbe_device.h"
#include "lbe_common.h"
#include "lbe_platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODEL_GENERIC (-1)

void print_usage(int model) {
	int generic = (model == MODEL_GENERIC);
	int is_1420 = (model == LBE_1420);
	int is_mini = (model == LBE_MINI);
	int is_1421 = (model == LBE_1421_DUALOUT);
	unsigned long mf =
		is_1420 ? LBE_1420_MAX_FREQ :
		is_mini ? LBE_MINI_MAX_FREQ :
		          LBE_1421_MAX_FREQ;

	printf("Usage: lbe-142x [OPTIONS]\n");
	printf("Options:\n");
	printf("  --help                 Show this help\n");
	printf("  --pid <0xNNNN>         Select a specific LBE device when more than one is attached\n");
	printf("                         (0x2443=1420, 0x2444=1421, 0x226f=1423, 0x2269=1425, 0x2211=Mini)\n");

	if (generic)
		printf("  --f1 <Hz>              Set OUT1 frequency, save to flash (1420 <=%lu, 1421 <=%lu, Mini <=%lu)\n",
		       LBE_1420_MAX_FREQ, LBE_1421_MAX_FREQ, LBE_MINI_MAX_FREQ);
	else
		printf("  --f1 <Hz>              Set OUT1 frequency (1-%lu Hz), save to flash\n", mf);

	if (generic || !is_mini)
		printf("  --f1t <Hz>             Set OUT1 temporary frequency%s\n",
		       generic ? " (not supported on Mini)" : "");

	if (generic || is_1421) {
		printf("  --f2 <Hz>              Set OUT2 frequency, save to flash%s\n",
		       generic ? " (LBE-1421/1423 only)" : "");
		printf("  --f2t <Hz>             Set OUT2 temporary frequency%s\n",
		       generic ? " (LBE-1421/1423 only)" : "");
	}

	printf("  --out <0|1>            Enable or disable outputs\n");

	if (generic || !is_mini)
		printf("  --pll <0|1>            Set PLL(0) or FLL(1) mode%s\n",
		       generic ? " (not supported on Mini)" : "");

	if (generic || is_1421)
		printf("  --pps <0|1>            Enable or disable 1PPS on OUT1%s\n",
		       generic ? " (LBE-1421/1423 only)" : "");

	printf("  --pwr1 <0|1>           Set OUT1 power level: normal(0) or low(1)\n");

	if (generic || is_1421)
		printf("  --pwr2 <0|1>           Set OUT2 power level: normal(0) or low(1)%s\n",
		       generic ? " (LBE-1421/1423 only)" : "");

	if (generic || is_mini)
		printf("  --drive <8|16|24|32>   Set OUT1 drive strength in mA%s\n",
		       generic ? " (Mini only)" : "");

	printf("  --blink                Blink output LED(s) for 3 seconds\n");
	printf("  --status               Display current device status\n");

	if (generic || is_mini || is_1421) {
		printf("  --monitor              Live GPS display (UTC, lat/lon, altitude, CNR bars)%s\n",
		       generic ? " (Mini: UBX; 1421/1423: NMEA via CDC)" : "");
	}
	if (generic || is_1421) {
		printf("  --port <name>          CDC port for --monitor (e.g. COM12 or /dev/ttyACM0)%s\n",
		       generic ? " (LBE-1421/1423)" : "");
	}
	if (generic || is_mini) {
		printf("  --gps-info             Print u-blox GPS module SW/HW version (UBX-MON-VER)%s\n",
		       generic ? " (Mini only)" : "");
	}
}

int main(int argc, char *argv[]) {
	struct lbe_device *dev;
	struct lbe_status status;
	enum lbe_model model;
	int changed = 0;
	uint16_t preferred_pid = 0;
	int help_requested = 0;

	printf("lbe-142x v1.2 20 Apr 2026 Leo Bodnar LBE-142x / LBE-Mini GPS clock source config\n");

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
		print_usage(dev ? (int)lbe_get_model(dev) : MODEL_GENERIC);
		if (dev) lbe_close_device(dev);
		return 0;
	}

	if (!dev) {
		fprintf(stderr, "\n");
		print_usage(MODEL_GENERIC);
		return 1;
	}

	model = lbe_get_model(dev);

	if (argc == 1) {
		print_usage(model);
		lbe_close_device(dev);
		return 1;
	}

	/* Several dual-output models (1421/1423/1425) share the LBE_1421_DUALOUT
	 * ops vtable, so name the device from its actual PID rather than the
	 * coarse model enum. */
	const char *model_name;
	switch (lbe_get_pid(dev)) {
	case PID_LBE_1420: model_name = "1420"; break;
	case PID_LBE_1421: model_name = "1421 dual output"; break;
	case PID_LBE_1423: model_name = "1423 dual output"; break;
	case PID_LBE_1425: model_name = "1425 dual output"; break;
	case PID_LBE_MINI: model_name = "Mini"; break;
	default:           model_name = "142x"; break;
	}
	printf("Connected to LBE-%s\n", model_name);

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--f1") == 0 || strcmp(argv[i], "--f2") == 0 || 
			strcmp(argv[i], "--f1t") == 0 || strcmp(argv[i], "--f2t") == 0) {
			if (i + 1 < argc) {
				int out_no = (argv[i][3] == '1') ? 1 : 2;
				int temp = (argv[i][4] == 't');
				
				if (out_no == 2 && (model == LBE_1420 || model == LBE_MINI)) {
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
			if (model != LBE_1421_DUALOUT) {
				fprintf(stderr, "1PPS on OUT1 control is only supported on LBE-1421\n");
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
			if (out_no == 2 && (model == LBE_1420 || model == LBE_MINI)) {
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
		} else if (strcmp(argv[i], "--blink") == 0) {
			if (lbe_blink_leds(dev) == 0) {
				printf("  Blink LED(s)\n");
				changed = 1;
			}
		} else if (strcmp(argv[i], "--status") == 0) {
			if (lbe_get_device_status(dev, &status) == 0) {
				printf("Device Status (0x%02X):\n", status.raw_status);
				printf("  GPS Lock: %s\n", (status.raw_status & LBE_GPS_LOCK_BIT) ? "Yes" : "No");
				printf("  PLL Lock: %s\n", status.pll_locked ? "Yes" : "No");
				/* Antenna status is not decodable on the Mini feature
				 * report -- the vendor UI does not expose it either. */
				if (model != LBE_MINI) {
					printf("  Antenna: %s\n", status.antenna_ok ? "OK" : "Short Circuit");
				}
				printf("  Output(s) Enabled: %s\n", status.outputs_enabled ? "Yes" : "No");
				printf("  OUT1 Frequency: %u Hz\n", status.frequency1);
				if (model == LBE_MINI) {
					printf("  OUT1 Drive Strength: %umA\n", status.out1_drive_ma);
					printf("  Signal loss count: %u\n", status.signal_loss_count);
				} else {
					printf("  OUT1 Power Level: %s\n", status.out1_power_low ? "Low" : "Normal");
				}

				if (model == LBE_1421_DUALOUT) {
					printf("  OUT2 Frequency: %u Hz\n", status.frequency2);
					printf("  OUT2 Power Level: %s\n", status.out2_power_low ? "Low" : "Normal");

					printf("  1PPS on OUT1: %s\n", status.pps_enabled ? "Enabled" : "Disabled");
				}
				/* Mini has no FLL/PLL mode toggle. */
				if (model != LBE_MINI) {
					printf("  Mode: %s\n", status.fll_enabled ? "FLL" : "PLL");
				}
			}
		} else if (strcmp(argv[i], "--monitor") == 0) {
			lbe_monitor(dev);
			changed = 1;
		} else if (strcmp(argv[i], "--gps-info") == 0) {
			lbe_gps_info(dev);
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
			print_usage(model);
		}
	}

	if (!changed) {
		printf("No changes made\n");
	}

	lbe_close_device(dev);
	return 0;
}