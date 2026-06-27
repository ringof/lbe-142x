/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifndef UBX_H
#define UBX_H

#include <stdint.h>
#include <stddef.h>
#include "gnss_view.h"

/* Shared u-blox UBX decoding. The LBE-Mini streams UBX out of its HID
 * interrupt endpoint; the LBE-1425 streams the same messages on its EP 0x83
 * diagnostics endpoint (behind a different HID frame header). Each model
 * strips its own framing and feeds the raw UBX bytes here. */

/* UBX NAV-CLOCK (class 0x01, id 0x22): receiver clock solution. This is the
 * disciplining/stability telemetry the LBE-1425 "diagnostics" view shows. */
struct ubx_clock {
	int      valid;
	int32_t  clkb_ns;    /* clock bias (ns) */
	int32_t  clkd_nsps;  /* clock drift (ns/s) */
	uint32_t tacc_ns;    /* time accuracy estimate (ns) */
	uint32_t facc_ps;    /* frequency accuracy estimate (ps/s) */
};

/* Largest UBX payload treated as plausible while scanning; a longer length
 * field is garbage and triggers a resync. Covers MON-VER + extensions and
 * NAV-SAT with many satellites. */
#define UBX_MAX_PAYLOAD 1024

/* Optional per-scan counters (pass NULL to ignore). Used by the Mini monitor's
 * frame-health display; the other callers pass NULL. */
struct ubx_scan_stats {
	uint32_t resyncs;    /* non-padding garbage bytes scanned past */
	uint32_t ck_fails;   /* Fletcher-8 mismatches */
	uint32_t pad_skips;  /* 0x00 / 0xFF inter-message padding scanned past */
};

/* Extract the next complete, checksum-valid UBX message from buf[*off .. len).
 * On success sets cls/id/payload/plen, advances *off past the message, and
 * returns 1. Returns 0 when no further complete message is present, leaving
 * *off at the first unconsumed byte (start of a partial message, or the scan
 * position) so the caller can memmove the remainder to the front. `st` may be
 * NULL. This is the single B5/62 + length + Fletcher-8 scanner shared by
 * ubx_consume() and the per-model MON-VER / CFG-GNSS / antenna probes. */
int ubx_next(const uint8_t *buf, size_t len, size_t *off,
             uint8_t *cls, uint8_t *id, const uint8_t **payload, uint16_t *plen,
             struct ubx_scan_stats *st);

/* Fletcher-8 checksum over a full UBX frame (sync..payload..ck), total bytes.
 * Returns 1 if the two trailing checksum bytes match. */
int ubx_checksum_ok(const uint8_t *msg, size_t total);

/* Decode individual UBX payloads (pointer past the 6-byte header, payload
 * length n). No-ops if n is too short. */
void ubx_parse_pvt(const uint8_t *p, int n, struct gnss_pvt *pvt);
void ubx_parse_sat(const uint8_t *p, int n, struct gnss_svinfo *sv);
void ubx_parse_clock(const uint8_t *p, int n, struct ubx_clock *clk);

/* Append `in` (in_len already-de-framed UBX bytes) to the reassembly buffer
 * `buf` (capacity buf_cap, current length *buf_len), then extract every
 * complete checksum-valid UBX message, dispatching NAV-PVT/NAV-SAT/NAV-CLOCK
 * into the provided structs (any may be NULL). Consumed bytes are removed from
 * the front of buf. Returns the number of NAV-PVT updates applied (so a caller
 * can trigger a redraw). On buffer overflow the buffer is reset. */
int ubx_consume(uint8_t *buf, size_t *buf_len, size_t buf_cap,
                const uint8_t *in, size_t in_len,
                struct gnss_pvt *pvt, struct gnss_svinfo *sv,
                struct ubx_clock *clk);

#endif
