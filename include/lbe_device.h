/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifndef LBE_DEVICE_H
#define LBE_DEVICE_H

#include <stdint.h>

struct lbe_device;

enum lbe_model {
	LBE_1420 = 0,
	LBE_1421_DUALOUT, // dual output
	LBE_MINI          // Mini Precision GPS Reference Clock
};

struct lbe_status {
    uint8_t raw_status;
    uint32_t frequency1;
    uint32_t frequency2;
    int outputs_enabled;
    int fll_enabled;
    int pll_locked;
    int antenna_ok;
    int pps_enabled;
    int out1_power_low;
    int out2_power_low;
    /* Mini only: actual output drive strength in mA (8/16/24/32).
     * 0 for other models which use the out1_power_low bool instead. */
    uint8_t out1_drive_ma;
    /* Mini only: firmware's running "Signal loss count" (what the vendor
     * GUI shows). 0 for other models. */
    uint8_t signal_loss_count;
};

/* Open an LBE device. Pass preferred_pid=0 to auto-select the only
 * attached device; pass a specific PID (e.g. 0x2444 for LBE-1421) to
 * disambiguate when more than one is plugged in. */
struct lbe_device* lbe_open_device(uint16_t preferred_pid);
void lbe_close_device(struct lbe_device* dev);
enum lbe_model lbe_get_model(struct lbe_device* dev);
/* The exact USB product id the device enumerated with. Lets callers
 * distinguish models that share an ops vtable (1421/1423/1425). */
uint16_t lbe_get_pid(struct lbe_device* dev);

/* Maximum settable frequency in Hz for the given output (1 or 2). Most
 * models are symmetric, but the LBE-1425 caps OUT1 at 800 MHz while OUT2
 * reaches 1.4 GHz, so callers must validate per output rather than using
 * a single device-wide limit. */
uint32_t lbe_max_freq(struct lbe_device* dev, int output);
int lbe_get_device_status(struct lbe_device* dev, struct lbe_status* status);
int lbe_set_frequency(struct lbe_device* dev, int output, uint32_t frequency);
int lbe_set_outputs_enable(struct lbe_device* dev, int enable);
int lbe_blink_leds(struct lbe_device* dev);
int lbe_set_frequency_temp(struct lbe_device* dev, int output, uint32_t frequency);
int lbe_set_pll_mode(struct lbe_device* dev, int fll_mode);
int lbe_set_1pps(struct lbe_device* dev, int enable);
int lbe_set_power_level(struct lbe_device* dev, int output, int low_power);

/* Mini only: set OUT1 drive strength to 8, 16, 24, or 32 mA. Returns -1 on
 * unsupported model or invalid value. */
int lbe_set_drive_ma(struct lbe_device* dev, unsigned ma);

/* Blocking live-monitor of the Mini's NAV stream (UTC, lat/lon, altitude,
 * fix type, per-satellite CNR bars). Returns -1 if the model doesn't
 * support monitoring. Otherwise loops until the process is killed. */
int lbe_monitor(struct lbe_device* dev);

/* Query the u-blox GPS module's UBX-MON-VER (SW / HW / extensions) and
 * print to stdout. Returns -1 if unsupported on this model. */
int lbe_gps_info(struct lbe_device* dev);

/* Reverse-engineering helper: claim the HID interface and hex-dump every
 * interrupt-IN frame from endpoint `ep` for `duration_ms` milliseconds.
 * Useful to check whether a device streams NMEA / UBX on the HID side.
 * Returns 0 on success. */
int lbe_rawdump(struct lbe_device* dev, uint8_t ep, int duration_ms);

#endif // LBE_DEVICE_H
