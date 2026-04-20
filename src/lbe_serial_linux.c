/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifdef __linux__

#include "lbe_serial.h"

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct lbe_serial {
	int  fd;
	char rx[1024];
	int  rx_len;
};

struct lbe_serial *lbe_serial_open(const char *path) {
	int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return NULL;
	}

	struct termios t;
	if (tcgetattr(fd, &t) < 0) {
		perror("tcgetattr");
		close(fd);
		return NULL;
	}
	cfmakeraw(&t);
	cfsetispeed(&t, B9600);
	cfsetospeed(&t, B9600);
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cc[VMIN]  = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr(fd, TCSANOW, &t);
	tcflush(fd, TCIFLUSH);

	struct lbe_serial *s = calloc(1, sizeof *s);
	if (!s) { close(fd); return NULL; }
	s->fd = fd;
	return s;
}

void lbe_serial_close(struct lbe_serial *s) {
	if (!s) return;
	if (s->fd >= 0) close(s->fd);
	free(s);
}

int lbe_serial_get_dcd(struct lbe_serial *s) {
	if (!s) return -1;
	int status = 0;
	if (ioctl(s->fd, TIOCMGET, &status) < 0) return -1;
	return (status & TIOCM_CAR) ? 1 : 0;
}

static long elapsed_ms(const struct timeval *start) {
	struct timeval now;
	gettimeofday(&now, NULL);
	return (now.tv_sec - start->tv_sec) * 1000L +
	       (now.tv_usec - start->tv_usec) / 1000L;
}

int lbe_serial_readline(struct lbe_serial *s, char *buf, size_t n,
                        int timeout_ms) {
	if (!s || !buf || n < 2) return -1;
	struct timeval start;
	gettimeofday(&start, NULL);
	while (1) {
		char *nl = memchr(s->rx, '\n', (size_t)s->rx_len);
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
		long left = timeout_ms - elapsed_ms(&start);
		if (left <= 0) return 0;

		struct timeval tv = { left / 1000, (left % 1000) * 1000 };
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(s->fd, &rfds);
		int rc = select(s->fd + 1, &rfds, NULL, NULL, &tv);
		if (rc < 0) return -1;
		if (rc == 0) return 0;

		if (s->rx_len >= (int)sizeof s->rx - 1) s->rx_len = 0;
		ssize_t got = read(s->fd, s->rx + s->rx_len,
		                   (size_t)(sizeof s->rx - (size_t)s->rx_len));
		if (got < 0) return -1;
		if (got > 0) s->rx_len += (int)got;
	}
}

int lbe_serial_find_nmea(char *out, size_t n) {
	/* Scan /dev/ttyACM0..9 in order. */
	for (int i = 0; i < 10; i++) {
		char path[32];
		snprintf(path, sizeof path, "/dev/ttyACM%d", i);
		if (access(path, R_OK | W_OK) != 0) continue;

		struct lbe_serial *s = lbe_serial_open(path);
		if (!s) continue;

		struct timeval t0;
		gettimeofday(&t0, NULL);
		int found = 0;
		while (elapsed_ms(&t0) < 1000) {
			char line[256];
			int r = lbe_serial_readline(s, line, sizeof line, 200);
			if (r > 2 && line[0] == '$' && line[1] == 'G') {
				found = 1;
				break;
			}
		}
		lbe_serial_close(s);
		if (found) {
			snprintf(out, n, "%s", path);
			return 0;
		}
	}
	return -1;
}

#endif
