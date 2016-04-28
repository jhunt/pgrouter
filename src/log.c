/*
  Copyright (c) 2016 James Hunt

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
 */

#include "pgrouter.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <syslog.h>
#include <time.h>

static int LOGLEVEL;

void pgr_logger(int level)
{
	switch (level) {
	case LOG_EMERG:
	case LOG_ALERT:
	case LOG_CRIT:
	case LOG_ERR:
		LOGLEVEL = LOG_ERR;
		break;

	case LOG_WARNING:
	case LOG_NOTICE:
	case LOG_INFO:
		LOGLEVEL = LOG_INFO;
		break;

	case LOG_DEBUG:
		LOGLEVEL = LOG_DEBUG;
		break;

	default:
		LOGLEVEL = LOG_ERR;
	}
}

static void _vlogf(FILE *io, const char *fmt, va_list ap)
{
	char *msg, tstamp[128];
	int n;
	struct tm t;
	time_t now = time(NULL);

	if (gmtime_r(&now, &t) == NULL) {
		pgr_abort(ABORT_UNKNOWN);
	}

	n = strftime(tstamp, sizeof(tstamp), "%c", &t);
	if (n < 0) {
		pgr_abort(ABORT_UNKNOWN);
	}

	n = vasprintf(&msg, fmt, ap);
	if (n < 0) {
		pgr_abort(ABORT_MEMFAIL);
	}

	fprintf(io, "[%s] %s\n", tstamp, msg);
	free(msg);
}

static void _vdlogf(FILE *io, const char *file, int line, const char *fn, const char *fmt, va_list ap)
{
	char *msg, tstamp[128];
	int n;
	struct tm t;
	time_t now = time(NULL);

	if (gmtime_r(&now, &t) == NULL) {
		pgr_abort(ABORT_UNKNOWN);
	}

	n = strftime(tstamp, sizeof(tstamp), "%c", &t);
	if (n < 0) {
		pgr_abort(ABORT_UNKNOWN);
	}

	n = vasprintf(&msg, fmt, ap);
	if (n < 0) {
		pgr_abort(ABORT_MEMFAIL);
	}

	fprintf(io, "[%s] DEBUG %s:%d %s() - %s\n", tstamp, file, line, fn, msg);
	free(msg);
}

void pgr_msgf(FILE *io, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	_vlogf(io, fmt, ap);
	va_end(ap);
}

void pgr_logf(FILE *io, int level, const char *fmt, ...)
{
	if (level <= LOGLEVEL) {
		va_list ap;
		va_start(ap, fmt);
		_vlogf(io, fmt, ap);
		va_end(ap);
	}
}

void pgr_dlogf(FILE *io, int level, const char *file, int line, const char *fn, const char *fmt, ...)
{
	if (level <= LOGLEVEL) {
		va_list ap;
		va_start(ap, fmt);
		_vdlogf(io, file, line, fn, fmt, ap);
		va_end(ap);
	}
}

void pgr_vlogf(FILE *io, int level, const char *fmt, va_list ap)
{
	if (level <= LOGLEVEL) {
		_vlogf(io, fmt, ap);
	}
}

void pgr_vdlogf(FILE *io, int level, const char *file, int line, const char *fn, const char *fmt, va_list ap)
{
	if (level <= LOGLEVEL) {
		_vdlogf(io, file, line, fn, fmt, ap);
	}
}

void pgr_hexdump_irl(const void *buf, size_t len)
{
	if (LOGLEVEL > LOG_DEBUG) {
		return;
	}

	char *xdig = "0123456789abcdef";
	char hex[47+1];
	char asc[16+1];
	size_t x, y;

	hex[47] = asc[16] = '\0';
	for (y = 0; y < len; y += 16) {
		memset(hex, ' ', 47);
		memset(asc, ' ', 16);
		for (x = 0; x < 16 && y + x < len; x++) {
			char c = ((char*)buf)[y+x];
			hex[x*3+0] = xdig[(c & 0xf0) >> 4];
			hex[x*3+1] = xdig[(c & 0x0f)];
			asc[x] = isprint(c) ? c : '.';
		}
		pgr_debugf("%08lo | %s | %s", y, hex, asc);
	}
}
