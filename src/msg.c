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

#ifdef PTEST
#include <strings.h>

#define so(s,x) do {\
	errno = 0; \
	if (x) { \
		fprintf(stderr, "%s ... OK\n", s); \
	} else { \
		fprintf(stderr, "FAIL: %s [!(%s)]\n", s, #x); \
		if (errno != 0) { \
			fprintf(stderr, "%s (errno %d)\n", strerror(errno), errno); \
		} \
		exit(1); \
	} \
} while (0)

#define m_should_be(s,m,offset,unread,used,free) do {\
	so(s ", offset should be " #offset, pgr_m_offset(m) == offset); \
	so(s ", unread should be " #unread, pgr_m_unread(m) == unread); \
	so(s ", used should be "   #used,   pgr_m_used(m) == used); \
	so(s ", free should be "   #free,   pgr_m_free(m) == free); \
} while (0)

#define msg_should_be(s,m,type,len) do {\
	so(s ", message type should be " #type, pgr_m_u8_at(m,0) == (type)); \
	so(s ", length should be "       #len,  pgr_m_u32_at(m,1) == (len)); \
} while (0)

#define writeok(f,s,n) so("writing " #n " bytes to fd `" #f "`", \
		write((f), (s), (n)) == (n))

#define ok(x)    so(#x " should succeed", (x) == 0)
#define notok(x) so(#x " should fail", (x) != 0)

#define null(x)    so(#x " should be NULL", (x) == NULL)
#define notnull(x) so(#x " should not be NULL", (x) != NULL)

int main(int argc, char **argv)
{
	int rc, in, out;
	MSG *m;
	FILE *inf, *outf;

	int i;
	char *s;

	inf = tmpfile();
	outf = tmpfile();
	if (inf == NULL || outf == NULL) {
		fprintf(stderr, "tmpfile() call failed: %s (errno %d)\n", strerror(errno), errno);
		exit(1);
	}
	in = fileno(inf);
	out = fileno(outf);

	m = pgr_m_new();
	so("pgr_m_new() yields a valid pointer", m != NULL);
	m_should_be("initial MSG", m, 0, 0, 0, MSG_BUFSIZ);

#define STR28 "seilohp3Eel7iesheik0sheing3d"
	writeok(in, "x\0\0\0\x8" "abcd", 9);
	writeok(in, "y\0\0\0\x21" STR28 "\0", 34);
	lseek(in, 0, SEEK_SET);

	ok(pgr_m_next(m, in));
	m_should_be("after pgr_m_next", m, 0, 9+34, 9+34, MSG_BUFSIZ-9-34);
	msg_should_be("after pgr_m_next", m, 'x', 8);
	so("message data should be \"abcd\"", memcmp(pgr_m_str_at(m, 5), "abcd", 4) == 0);
	pgr_m_discard(m, in);

	ok(pgr_m_next(m, in));
	m_should_be   ("next message", m, 0, 34, 34, MSG_BUFSIZ-34);
	msg_should_be ("next message", m, 'y', 33);
	so("message data should be \"" STR28 "\"", memcmp(pgr_m_str_at(m, 5), STR28, 28) == 0);
	pgr_m_discard(m, in);

	notok(pgr_m_next(m, in));
#undef STR28

	ftruncate(in, 0);
	lseek(in, 0, SEEK_SET);
	writeok(in, "L\0\0\x80\x0", 5);
	notnull(s = malloc(0x8000));
	memset(s, '.', 0x8000);
	writeok(in, s, 0x8000);
	so("first message should be larger than 16k", lseek(in, 0, SEEK_CUR) > MSG_BUFSIZ);
	writeok(in, "S\0\0\0\x4", 5);
	lseek(in, 0, SEEK_SET);

	ok(pgr_m_next(m, in));
	m_should_be("large message", m, 0, MSG_BUFSIZ, MSG_BUFSIZ, 0);
	msg_should_be("large message", m, 'L', 0x8000);
	pgr_m_discard(m, in);
	m_should_be("post-discard", m, 0, 0, 0, MSG_BUFSIZ);

	ok(pgr_m_next(m, in));
	m_should_be("small message", m, 0, 5, 5, MSG_BUFSIZ-5);
	msg_should_be("small message", m, 'S', 5);

	fclose(inf);
	fclose(outf);

	printf("PASS\n");
	return 0;
}
#endif
