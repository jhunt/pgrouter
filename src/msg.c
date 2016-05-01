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

static unsigned int size(MBUF *m)
{
	if (m->fill < 5) {
		return 0;
	}

	if (m->buf[0] == 0) {
		return (m->buf[0] << 24)
		     | (m->buf[1] << 16)
		     | (m->buf[2] <<  8)
		     | (m->buf[3]);
	}
	return ((m->buf[1] << 24)
	      | (m->buf[2] << 16)
	      | (m->buf[3] <<  8)
	      | (m->buf[4]))
	      + 1; /* for the type! */
}

/* Generate a new MBUF structure of the given size,
   allocated on the heap. The `len` argument must be
   at least 16 (octets). */
MBUF* pgr_mbuf_new(size_t len)
{
	MBUF *m = malloc(sizeof(MBUF) + len);
	if (!m) {
		pgr_abort(ABORT_MEMFAIL);
	}
	memset(m, 0, sizeof(MBUF) + len);
	m->len = len;
	return m;
}

/* Set the input and output file descriptors to the
   passed values.  To leave existing fd untouched,
   specify the constant `MBUF_SAME_FD`.  To unset a
   descriptor, specify `MBUF_NO_FD`. */
int pgr_mbuf_setfd(MBUF *m, int in, int out)
{
	assert(m != NULL);
	assert(in == MBUF_SAME_FD || in == MBUF_NO_FD || in >= 0);
	assert(out == MBUF_SAME_FD || out == MBUF_NO_FD || out >= 0);

	if (in != MBUF_SAME_FD) {
		m->infd = in;
	}
	if (out != MBUF_SAME_FD) {
		m->outfd = out;
	}
	return 0;
}

/* Fill as much of the buffer with octets read from
   the input file descriptor.  If the buffer already
   contains enough data to see the first 5 octets of
   a message, this call does nothing and returns
   immediately. */
int pgr_mbuf_recv(MBUF *m)
{
	while (m->fill < 5) {
		ssize_t n = read(m->infd, m->buf + m->fill, m->len - m->fill);
		if (n <= 0) {
			return (int)n;
		}
		m->fill += n;
	}
	return m->fill;
}

/* Send the first message in the buffer to the output
   file descriptor, buffering all data sent, so that
   it can be resent later.  For very large messages,
   i.e. INSERT statements with large blobs), this may
   require reading from the input file descriptor. */
int pgr_mbuf_send(MBUF *m)
{
	return 0;
}

/* Relay the first message in the buffer to the output
   file descriptor, and reposition the buffer at the
   beginning of the next message.  This may lead to an
   empty buffer. */
int pgr_mbuf_relay(MBUF *m)
{
	int len, wr_ok;
	ssize_t n, off;

	if (m->fill < 5) {
		return 1;
	}

	wr_ok = 1;
	len = size(m);
	while (len > m->fill) {
		off = 0;
		while (wr_ok && off < m->fill) {
			fprintf(stderr, "writing %lib to outfd %d\n", m->fill - off, m->outfd);
			n = write(m->outfd, m->buf + off, m->fill - off);
			wr_ok = (n > 0);
			off += n;
		}

		len -= m->fill;
		m->fill = 0;

		n = read(m->infd, m->buf, m->len);
		if (n <= 0) {
			return (int)n;
		}
		m->fill += n;
	}

	if (len > 0) {
		off = 0;
		while (wr_ok && off < len) {
			n = write(m->outfd, m->buf + off, (len - off));
			wr_ok = (n > 0);
			off += n;
		}

		memmove(m->buf, m->buf + len, m->fill - len);
		m->fill -= len;
	}
	return 0;
}

/* Discard all buffered data for the current message,
   reading (and discarding) from the input descriptor
   if necessary. */
int pgr_mbuf_discard(MBUF *m)
{
	int len;
	ssize_t n;

	if (m->fill < 5) {
		return 1;
	}

	len = size(m);
	while (len > m->fill) {
		len -= m->fill;
		m->fill = 0;

		n = read(m->infd, m->buf, m->len);
		if (n <= 0) {
			return (int)n;
		}
		m->fill += n;
	}

	if (len > 0) {
		memmove(m->buf, m->buf + len, m->fill - len);
		m->fill -= len;
	}

	return 0;
}

char pgr_mbuf_msgtype(MBUF *m)
{
	if (m->fill == 0) {
		return -1;
	}
	return m->buf[0];
}

unsigned int pgr_mbuf_msglength(MBUF *m)
{
	/* FIXME: doesn't handle untyped messages! */
	if (m->fill < 5) {
		return 0;
	}

	return (m->buf[1] << 24)
	     | (m->buf[2] << 16)
	     | (m->buf[3] <<  8)
	     | (m->buf[4]);
}

void* pgr_mbuf_data(MBUF *m)
{
	if (m->fill < 5) {
		return NULL;
	}
	return m->buf + 5;
}

#ifdef PTEST
#include <strings.h>

#define so(s,x) do {\
	errno = 0; \
	if (x) { \
		fprintf(stderr, "%s ... OK\n", s); \
	} else { \
		fprintf(stderr, "%s:%d: FAIL: %s [!(%s)]\n", __FILE__, __LINE__, s, #x); \
		if (errno != 0) { \
			fprintf(stderr, "%s (errno %d)\n", strerror(errno), errno); \
		} \
		exit(1); \
	} \
} while (0)

#define is(x,n)    so(#x " should equal " #n, (x) == (n))
#define isnt(x,n)  so(#x " should not equal " #n, (x) == (n))
#define ok(x)      so(#x " should succeed", (x) == 0)
#define notok(x)   so(#x " should fail", (x) != 0)
#define null(x)    so(#x " should be NULL", (x) == NULL)
#define notnull(x) so(#x " should not be NULL", (x) != NULL)

#define writeok(f,s,n) so("writing " #n " bytes to fd `" #f "`", \
		write((f), (s), (n)) == (n))

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

#define msg_is(s,m,t,l) do { \
	so(s ", message type should be "   #t, pgr_mbuf_msgtype(m) == (t)); \
	so(s ", message length should be " #l, pgr_mbuf_msglength(m) == (l)); \
} while (0)

int main(int argc, char **argv)
{
	int rc, in, out;
	MBUF *m;
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

	notnull(m = pgr_mbuf_new(512));
	ok(pgr_mbuf_setfd(m, in, out));
	so("initial fill offset should be 0", m->fill == 0);
	so("discarding with an empty buffer returns an error",
			pgr_mbuf_discard(m) != 0);
	so("interrogating message type from an empty buffer returns an error",
			pgr_mbuf_msgtype(m) == -1);
	so("interrogating message length from an empty buffer returns a zero-length",
			pgr_mbuf_msglength(m) == 0);
	so("interrogating message data from an empty buffer returns NULL",
			pgr_mbuf_data(m) == NULL);

#define RESET() do {\
	ftruncate(in, 0); lseek(in, 0, SEEK_SET); \
	ftruncate(out, 0); lseek(out, 0, SEEK_SET); \
	m->fill = 0; \
} while (0)

	/***   Read & Discard   ****************************************************/
	RESET();
	writeok(in, "x\0\0\0\x8" "abcd", 9);
	writeok(in, "y\0\0\0\x21" "abcdefghijklmnopqrstuvwxyz!!" "\0", 34);
	lseek(in, 0, SEEK_SET);

	is(pgr_mbuf_recv(m), 9+34);
	so("mbuf fill offset should be 9+34", m->fill == 9+34);
	msg_is("for first message", m, 'x', 8);
	ok(memcmp(pgr_mbuf_data(m), "abcd", 4));

	ok(pgr_mbuf_discard(m));
	so("mbuf fill offset should be 34", m->fill == 34);

	is(pgr_mbuf_recv(m), 34);
	so("mbuf fill offset should be 34", m->fill == 34);
	msg_is("for second message", m, 'y', 33);
	ok(memcmp(pgr_mbuf_data(m), "abcdefghijklmnopqrstuvwxyz!!", 28));

	ok(pgr_mbuf_discard(m));
	so("mbuf fill offset should be 0", m->fill == 0);

	is(pgr_mbuf_recv(m), 0); /* eof */

	/***   Read & Relay   ******************************************************/
	RESET();
	writeok(in, "A\0\0\0\x9" "FIRST",  10);
	writeok(in, "B\0\0\0\xa" "SECOND", 11);
	lseek(in, 0, SEEK_SET);

	is(pgr_mbuf_recv(m), 10+11);
	ok(pgr_mbuf_relay(m));
	lseek(out, 0, SEEK_SET);

	notnull(s = malloc(1024));
	is(read(out, s, 1024), 10);
	ok(memcmp(s, "A\0\0\0\x9" "FIRST", 10));
	free(s);

	msg_is("after relay of first message", m, 'B', 10);



	/***   Read & Discard (LARGE MESSAGES)   ***********************************/
	RESET();
	writeok(in, "L\0\0\x80\x04", 5);
	notnull(s = malloc(0x8000));
	memset(s, '.', 0x8000);
	writeok(in, s, 0x8000);
	free(s);
	so("first message should be larger than buffer", lseek(in, 0, SEEK_CUR) > m->len);

	writeok(in, "S\0\0\0\x4", 5);
	lseek(in, 0, SEEK_SET);

	is(pgr_mbuf_recv(m), m->len);
	msg_is("for large message", m, 'L', 0x8004);

	ok(pgr_mbuf_discard(m));
	so("mbuf fill offset should be 5", m->fill == 5);

	is(pgr_mbuf_recv(m), m->fill);
	msg_is("after discarding large message", m, 'S', 4);



	/***   Read & Relay (LARGE MESSAGES)   *************************************/
	RESET();
	writeok(in, "L\0\0\x80\x04", 5);
	notnull(s = malloc(0x8000));
	memset(s, '.', 0x8000);
	writeok(in, s, 0x8000);
	free(s);
	so("first message should be larger than buffer", lseek(in, 0, SEEK_CUR) > m->len);

	writeok(in, "S\0\0\0\x4", 5);
	lseek(in, 0, SEEK_SET);

	is(pgr_mbuf_recv(m), m->len);
	msg_is("for large message", m, 'L', 0x8004);
	ok(pgr_mbuf_relay(m));
	lseek(out, 0, SEEK_SET);

	notnull(s = malloc(0x8005));
	is(read(out, s, 0x8005), 0x8005);
	ok(memcmp(s, "L\0\0\x80\x04", 5));
	free(s);

	msg_is("after relay of first message", m, 'S', 4);

	fclose(inf);
	fclose(outf);

	printf("PASS\n");
	return 0;
}
#endif
