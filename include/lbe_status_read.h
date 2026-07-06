/*
 * SPDX-License-Identifier: MIT
 */
#ifndef LBE_STATUS_READ_H
#define LBE_STATUS_READ_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "lbe_transport.h"
#include "lbe_platform.h"

/* Shared status-read retry loop.  A valid status always carries a non-zero
 * frequency at `freq_offset` (LE32, min 1 Hz, realistically MHz).  An
 * all-zero report is a transient garbage read -- seen e.g. when an antenna
 * unplug briefly shorts and stuns the MCU.  Retry once (with a 20 ms sleep)
 * before giving up.
 *
 * static inline so the test binary (which does not link lbe_device.c or any
 * transport) can include it and resolve the symbols against local mocks. */
static inline int lbe_read_status_retrying(
    struct lbe_transport *t, uint8_t report_id,
    size_t freq_offset, uint8_t *buf, size_t buf_len)
{
	for (int attempt = 0; attempt < 2; attempt++) {
		memset(buf, 0, buf_len);
		if (lbe_transport_feat_get(t, report_id, buf) < 0) return -1;
		uint32_t f1 = (uint32_t)buf[freq_offset]
		            | ((uint32_t)buf[freq_offset + 1] << 8)
		            | ((uint32_t)buf[freq_offset + 2] << 16)
		            | ((uint32_t)buf[freq_offset + 3] << 24);
		if (f1 != 0) return 0;
		if (attempt == 0) lbe_sleep_ms(20);
	}
	return -1;   /* persistently implausible -- treat as failed read */
}

#endif
