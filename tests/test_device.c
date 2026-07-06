/*
 * SPDX-License-Identifier: MIT
 */
/* Tests for the shared status-read retry helper (lbe_status_read.h).
 * The helper is static inline, so it inlines into this translation unit and
 * resolves lbe_transport_feat_get / lbe_sleep_ms against the local mocks
 * defined here -- no conflict with other test files. */
#include "test_util.h"
#include "lbe_transport.h"

#include <stdint.h>
#include <string.h>

/* --- mocks ---------------------------------------------------------------- */

/* File-scoped mock state: controls what lbe_transport_feat_get returns. */
static int mock_call;          /* call counter (0-based) */
static int mock_fail;          /* if set, feat_get returns -1 */
static uint8_t mock_buf[60];   /* data returned on each call */
static uint8_t mock_buf2[60];  /* data returned on second call (if different) */
static int mock_use_buf2;      /* if set, second call returns mock_buf2 */

int lbe_transport_feat_get(struct lbe_transport *t, uint8_t report_id,
                           uint8_t *data) {
	(void)t; (void)report_id;
	if (mock_fail) return -1;
	if (mock_call > 0 && mock_use_buf2)
		memcpy(data, mock_buf2, 60);
	else
		memcpy(data, mock_buf, 60);
	mock_call++;
	return 0;
}

void lbe_sleep_ms(int ms) { (void)ms; }

/* --- include the static inline helper AFTER the mocks ------------------- */
#include "lbe_status_read.h"

/* Helper: write a LE32 into buf at offset. */
static void put_le32(uint8_t *buf, size_t off, uint32_t v) {
	buf[off]     = (uint8_t)(v);
	buf[off + 1] = (uint8_t)(v >> 8);
	buf[off + 2] = (uint8_t)(v >> 16);
	buf[off + 3] = (uint8_t)(v >> 24);
}

void run_device_tests(void) {
	printf("device:\n");
	uint8_t buf[60];

	/* (a) first read valid → no retry needed */
	mock_call = 0; mock_fail = 0; mock_use_buf2 = 0;
	memset(mock_buf, 0, sizeof mock_buf);
	put_le32(mock_buf, 6, 10000000u);   /* 10 MHz at freq_offset=6 */
	CHECK(lbe_read_status_retrying(NULL, 0x4B, 6, buf, sizeof buf) == 0);
	CHECK(mock_call == 1);   /* only one call needed */

	/* (b) first read zero, second valid → retry succeeds */
	mock_call = 0; mock_fail = 0; mock_use_buf2 = 1;
	memset(mock_buf, 0, sizeof mock_buf);
	memset(mock_buf2, 0, sizeof mock_buf2);
	put_le32(mock_buf2, 6, 10000000u);  /* valid on second call */
	CHECK(lbe_read_status_retrying(NULL, 0x4B, 6, buf, sizeof buf) == 0);
	CHECK(mock_call == 2);

	/* (c) both zero → returns -1 */
	mock_call = 0; mock_fail = 0; mock_use_buf2 = 0;
	memset(mock_buf, 0, sizeof mock_buf);
	CHECK(lbe_read_status_retrying(NULL, 0x4B, 6, buf, sizeof buf) == -1);
	CHECK(mock_call == 2);

	/* (d) transport error → returns -1 immediately */
	mock_call = 0; mock_fail = 1; mock_use_buf2 = 0;
	CHECK(lbe_read_status_retrying(NULL, 0x4B, 6, buf, sizeof buf) == -1);
	CHECK(mock_call == 0);   /* feat_get returned -1 before call++ */
}
