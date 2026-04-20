/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifdef _WIN32

#include "lbe_serial.h"
#include "lbe_platform.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lbe_serial {
	HANDLE h;
	char   rx[1024];
	int    rx_len;
};

struct lbe_serial *lbe_serial_open(const char *path) {
	/* COM1..COM9 work bare; COM10+ must use the \\.\ namespace. Just
	 * always prefix it; both forms accept \\.\COMx. */
	char full[64];
	_snprintf_s(full, sizeof full, _TRUNCATE, "\\\\.\\%s", path);

	HANDLE h = CreateFileA(full, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	                        OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "CreateFile %s: error %lu\n", path, GetLastError());
		return NULL;
	}

	DCB dcb;
	memset(&dcb, 0, sizeof dcb);
	dcb.DCBlength = sizeof dcb;
	if (!GetCommState(h, &dcb)) {
		fprintf(stderr, "GetCommState: %lu\n", GetLastError());
		CloseHandle(h);
		return NULL;
	}
	dcb.BaudRate     = CBR_9600;
	dcb.ByteSize     = 8;
	dcb.Parity       = NOPARITY;
	dcb.StopBits     = ONESTOPBIT;
	dcb.fBinary      = TRUE;
	dcb.fAbortOnError = FALSE;
	if (!SetCommState(h, &dcb)) {
		fprintf(stderr, "SetCommState: %lu\n", GetLastError());
		CloseHandle(h);
		return NULL;
	}

	COMMTIMEOUTS to;
	memset(&to, 0, sizeof to);
	/* Block the read up to 100 ms total; return as soon as any byte
	 * arrives. Avoid the MAXDWORD non-blocking special case which on
	 * some drivers returns 0 bytes forever when nothing is queued. */
	to.ReadIntervalTimeout        = 50;
	to.ReadTotalTimeoutConstant   = 100;
	to.ReadTotalTimeoutMultiplier = 0;
	SetCommTimeouts(h, &to);

	/* Drain any stale buffered bytes and raise DTR/RTS so the CDC
	 * device starts transmitting (some firmwares gate NMEA on DTR). */
	PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
	EscapeCommFunction(h, SETDTR);
	EscapeCommFunction(h, SETRTS);

	struct lbe_serial *s = calloc(1, sizeof *s);
	if (!s) { CloseHandle(h); return NULL; }
	s->h = h;
	return s;
}

void lbe_serial_close(struct lbe_serial *s) {
	if (!s) return;
	if (s->h && s->h != INVALID_HANDLE_VALUE) CloseHandle(s->h);
	free(s);
}

int lbe_serial_get_dcd(struct lbe_serial *s) {
	if (!s) return -1;
	DWORD ms = 0;
	if (!GetCommModemStatus(s->h, &ms)) return -1;
	return (ms & MS_RLSD_ON) ? 1 : 0;
}

int lbe_serial_readline(struct lbe_serial *s, char *buf, size_t n,
                        int timeout_ms) {
	if (!s || !buf || n < 2) return -1;
	DWORD deadline = GetTickCount() + (DWORD)timeout_ms;
	while (1) {
		/* Is there a line already in the buffer? */
		char *nl = (char *)memchr(s->rx, '\n', (size_t)s->rx_len);
		if (nl) {
			size_t line_len = (size_t)(nl - s->rx);
			if (line_len && s->rx[line_len - 1] == '\r') line_len--;
			if (line_len > n - 1) line_len = n - 1;
			memcpy(buf, s->rx, line_len);
			buf[line_len] = '\0';
			int consumed = (int)(nl - s->rx) + 1;
			memmove(s->rx, s->rx + consumed,
			        (size_t)(s->rx_len - consumed));
			s->rx_len -= consumed;
			return (int)line_len;
		}
		if (GetTickCount() >= deadline) return 0;

		if (s->rx_len >= (int)sizeof s->rx - 1) s->rx_len = 0;
		DWORD got = 0;
		BOOL ok = ReadFile(s->h, s->rx + s->rx_len,
		                   (DWORD)(sizeof s->rx - (size_t)s->rx_len),
		                   &got, NULL);
		if (!ok) return -1;
		if (got > 0) s->rx_len += (int)got;
	}
}

int lbe_serial_find_nmea(char *out, size_t n) {
	/* QueryDosDevice lists all defined NT device names; COM ports look
	 * like "COM12". Walk them; try each, sniff for "$G" within 1 s. */
	char dbg_buf[2];
	int debug = lbe_getenv("LBE_NMEA_DEBUG", dbg_buf, sizeof dbg_buf);
	char names[32768];
	DWORD len = QueryDosDeviceA(NULL, names, sizeof names);
	if (len == 0) { if (debug) fprintf(stderr, "QueryDosDevice: %lu\n", GetLastError()); return -1; }
	for (DWORD off = 0; off < len; ) {
		const char *name = names + off;
		size_t sl = strlen(name);
		if (sl == 0) break;
		off += (DWORD)(sl + 1);
		if (strncmp(name, "COM", 3) != 0) continue;
		if (debug) fprintf(stderr, "trying %s\n", name);

		struct lbe_serial *s = lbe_serial_open(name);
		if (!s) { if (debug) fprintf(stderr, "  open failed\n"); continue; }
		DWORD t0 = GetTickCount();
		int found = 0;
		while (GetTickCount() - t0 < 1500) {
			char line[256];
			int r = lbe_serial_readline(s, line, sizeof line, 200);
			if (debug && r > 0) fprintf(stderr, "  rx: %s\n", line);
			if (r > 2 && line[0] == '$' && line[1] == 'G') {
				found = 1;
				break;
			}
		}
		lbe_serial_close(s);
		if (found) {
			_snprintf_s(out, n, _TRUNCATE, "%s", name);
			return 0;
		}
	}
	return -1;
}

#endif
