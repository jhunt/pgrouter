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
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

MSG* pgr_m_new()
{
	MSG *m = calloc(1, sizeof(MSG));
	if (!m) {
		pgr_abort(ABORT_MEMFAIL);
	}
	m->free = sizeof(m->buf);
	return m;
}

void pgr_m_init(MSG *m)
{
	memset(m, 0, sizeof(MSG));
	m->free = sizeof(m->buf);
}

void pgr_m_skip(MSG *m, size_t n)
{
	assert(pgr_m_unread(m) >= n);
	m->offset += n;
}

void pgr_m_discard(MSG *m, int fd)
{
	int len;
	ssize_t n;

	if (pgr_m_u8_at(m,0) == 0) {
		len = pgr_m_u32_at(m,0);
	} else {
		len = 1 + pgr_m_u32_at(m,1);
	}

	n = pgr_m_unread(m);
	while (len > n) {
		/* there's more data on the wire.                 */
		/* drop what we've got, and then go read the rest */
		len -= n;

		m->offset = 0;
		m->free = MSG_BUFSIZ;
		n = read(fd, m->buf, m->free);
		if (n <= 0) {
			return;
		}
	}

	pgr_m_skip(m, len);
	pgr_m_flush(m);
}

void pgr_m_flush(MSG *m)
{
	if (m->offset > 0 && pgr_m_unread(m) > 0) {
		memmove(m->buf, m->buf + pgr_m_offset(m), pgr_m_unread(m));
	}
	m->free += m->offset;
	m->offset = 0;
}

int pgr_m_write(MSG *m, const void *buf, size_t len)
{
	if (len > m->free) {
		memcpy(m->buf + pgr_m_used(m), buf, m->free);
		len -= m->free;
		m->free = 0;
		return len;
	}

	memcpy(m->buf + pgr_m_used(m), buf, len);
	m->free -= len;
	return 0;


}

int pgr_m_sendn(MSG *m, int fd, size_t len)
{
	int rc;
	assert(len != 0);

	pgr_debugf("sending %d bytes to fd %d", len, fd);
	rc = pgr_sendn(fd, m->buf + m->offset, len);
	if (rc != 0) {
		return rc;
	}

	pgr_m_skip(m, len);
	return 0;
}

int pgr_m_resend(MSG *m, int fd)
{
	pgr_m_rewind(m);
	return pgr_m_send(m, fd);
}

int pgr_m_next(MSG *m, int fd)
{
	size_t n;

	while (pgr_m_unread(m) < 5) {
		n = read(fd, pgr_m_buffer(m), pgr_m_free(m));
		if (n <= 0) {
			return -1;
		}
		m->free -= n;
	}

	return 0;
}

int pgr_m_relay(MSG *m, int from, int to)
{
	size_t len = pgr_m_u32_at(m, 1) + 1;
	size_t n   = pgr_m_unread(m);

	/* is there more to this message? */
	while (len > pgr_m_unread(m)) {
		len -= pgr_m_unread(m);
		pgr_m_send(m, to);
		pgr_m_flush(m);
		pgr_m_next(m, from);
	}
	pgr_m_sendn(m, to, len);
	pgr_m_flush(m);

	return 0;
}

int pgr_m_ignore(MSG *m, int fd, const char *until)
{
	size_t n, len;
	char type;

	for (;;) {
		type = pgr_m_u8_at(m, 0);
		len  = pgr_m_u32_at(m, 1) + 1;
		n    = pgr_m_unread(m);

		while (len > n) {
			/* there's more data on the wire.                 */
			/* send what we've got, and then go read the rest */
			len -= n;

			m->offset = 0;
			m->free = MSG_BUFSIZ;
			n = read(fd, m->buf, m->free);
			if (n <= 0) {
				return -1;
			}
		}

		pgr_m_skip(m, len);
		pgr_m_flush(m);

		if (strchr(until, type) != NULL) {
			return 0;
		}

		pgr_m_next(m, fd);
	}
}

int pgr_m_iserror(MSG *m, const char *code)
{
	if (pgr_m_u8_at(m, 0) != 'E') {
		return 0;
	}

	char *e;

	e = pgr_m_str_at(m, 5);
	while (*e) {
		if (*e == 'C') {
			e++;
			return strcmp(e, code) == 0;
		}
		while (*e++)
			;
	}

	return 0;
}

void pgr_m_errorf(MSG *m, char *sev, char *code, char *msgf, ...)
{
	int len;
	va_list ap;
	char *msg;

	va_start(ap, msgf);
	vasprintf(&msg, msgf, ap);
	va_end(ap);

	len = 1+4                /* type + len            */
	    + 1+strlen(sev)+1    /* 'S' + severity + '\0' */
	    + 1+strlen(code)+1   /* 'C' + code     + '\0' */
	    + 1+strlen(msg)+1    /* 'M' + msg      + '\0' */
	    + 1;                 /* trailing '\0'         */
	len = htonl(len);
	pgr_m_write(m, "E",  1); pgr_m_write(m, &len, 4);
	pgr_m_write(m, "S",  1); pgr_m_write(m, sev,  strlen(sev)  + 1);
	pgr_m_write(m, "C",  1); pgr_m_write(m, code, strlen(code) + 1);
	pgr_m_write(m, "M",  1); pgr_m_write(m, msg,  strlen(msg)  + 1);
	pgr_m_write(m, "\0", 1);
}
