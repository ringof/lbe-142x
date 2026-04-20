/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#include "lbe_platform.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

void lbe_sleep_ms(int ms) {
	if (ms <= 0) return;
#ifdef _WIN32
	Sleep((DWORD)ms);
#else
	usleep((useconds_t)ms * 1000);
#endif
}

uint32_t lbe_millis(void) {
#ifdef _WIN32
	return (uint32_t)GetTickCount();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
#endif
}

int lbe_getenv(const char *name, char *out, size_t n) {
	if (!name || !out || n == 0) return 0;
	out[0] = '\0';

	const char *v;
#if defined(_MSC_VER)
	char   *buf = NULL;
	size_t  buf_len = 0;
	if (_dupenv_s(&buf, &buf_len, name) != 0 || !buf) return 0;
	v = buf;
#else
	v = getenv(name);
#endif

	int ok = 0;
	if (v && v[0]) {
		size_t copy = strlen(v);
		if (copy > n - 1) copy = n - 1;
		memcpy(out, v, copy);
		out[copy] = '\0';
		ok = 1;
	}
#if defined(_MSC_VER)
	free(buf);
#endif
	return ok;
}

void lbe_setenv(const char *name, const char *value) {
	if (!name || !value) return;
#if defined(_MSC_VER)
	_putenv_s(name, value);
#elif defined(_WIN32)
	/* MinGW: synthesise "NAME=VALUE" for putenv, which keeps the pointer.
	 * The leak is intentional -- env survives for the program's lifetime. */
	size_t nlen = strlen(name), vlen = strlen(value);
	char *kv = (char *)malloc(nlen + vlen + 2);
	if (!kv) return;
	memcpy(kv, name, nlen);
	kv[nlen] = '=';
	memcpy(kv + nlen + 1, value, vlen);
	kv[nlen + 1 + vlen] = '\0';
	putenv(kv);
#else
	setenv(name, value, 1);
#endif
}

void lbe_enable_vt(void) {
#ifdef _WIN32
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD mode;
	if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
		SetConsoleMode(h, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
	}
#endif
}
