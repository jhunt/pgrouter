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
#include <stdlib.h>
#include <string.h>

static int startup_message(MBUF *m, CONNECTION *c)
{
	PARAM *p;
	unsigned int len, n, off;

	len = 4 + 4 + 1;
	for (p = c->params; p; p = p->next) {
		len = len + strlen(p->name)  + 1
		          + strlen(p->value) + 1;
	}

	len = htonl(len);
	pgr_mbuf_cat(m, &len, 4);
	pgr_mbuf_cat(m, "\0\x3\0\0", 4);

	for (p = c->params; p; p = p->next) {
		pgr_mbuf_cat(m, p->name,  strlen(p->name)  + 1);
		pgr_mbuf_cat(m, p->value, strlen(p->value) + 1);
	}
	pgr_mbuf_cat(m, "\0", 1);
	return 0;
}

static int auth_md5_message(MBUF *m, CONNECTION *c)
{
	char buf[13];
	memcpy(buf, "R\0\0\0\xc\0\0\0\x5", 9);
	memcpy(buf+9, c->salt, 4);
	return pgr_mbuf_cat(m, buf, sizeof(buf));
}

static int auth_ok_message(MBUF *m, CONNECTION *c)
{
	return pgr_mbuf_cat(m, "R\0\0\0\x8\0\0\0\0", 9);
}

static int password_message(MBUF *m, CONNECTION *c)
{
	MD5 md5;
	char buf[41];

	pgr_md5_init(&md5);
	pgr_md5_update(&md5, c->pwhash, 32);
	pgr_md5_update(&md5, c->salt, 4);

	memcpy(buf, "p\0\0\0\x28md5", 8);
	pgr_md5_hex(buf+8, &md5);
	buf[40] = '\0';
	return pgr_mbuf_cat(m, buf, sizeof(buf));
}

static int ready_for_query(MBUF *m, CONNECTION *c)
{
	return pgr_mbuf_cat(m, "Z\0\0\0\x5I", 6);
}

static void error_response(MBUF *m, char *sev, char *code, char *msgf, ...)
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
	pgr_mbuf_cat(m, "E",  1); pgr_mbuf_cat(m, &len, 4);
	pgr_mbuf_cat(m, "S",  1); pgr_mbuf_cat(m, sev,  strlen(sev)  + 1);
	pgr_mbuf_cat(m, "C",  1); pgr_mbuf_cat(m, code, strlen(code) + 1);
	pgr_mbuf_cat(m, "M",  1); pgr_mbuf_cat(m, msg,  strlen(msg)  + 1);
	pgr_mbuf_cat(m, "\0", 1);
}
static PARAM* copy_params(PARAM *src)
{
	PARAM *dst = calloc(1, sizeof(PARAM));
	if (!dst) {
		pgr_abort(ABORT_MEMFAIL);
	}

	dst->name = strdup(src->name);
	dst->value = strdup(src->value);
	if (!dst->name || !dst->value) {
		pgr_abort(ABORT_MEMFAIL);
	}

	return dst;
}

static int extract_params(CONNECTION *c, MBUF *m)
{
	PARAM **p;
	char *x;

	/* find the next insertion point */
	for (p = &c->params; *p; p = &(*p)->next)
		;

	/* process parameter name/value pairs */
	for (x = pgr_mbuf_data(m, 4, 0); x && *x;) {
		*p = calloc(1, sizeof(PARAM));
		if (!*p) {
			pgr_abort(ABORT_MEMFAIL);
		}
		(*p)->name = strdup(x);
		x += strlen(x) + 1;
		if (!*x) {
			(*p)->value = strdup("");
			return -1;
		}
		(*p)->value = strdup(x);
		x += strlen(x) + 1;

		pgr_debugf("received startup parameter %s = '%s'",
				(*p)->name, (*p)->value);

		/* extract out 'user' and 'database' */
		if (strcmp((*p)->name, "user") == 0) {
			pgr_debugf("recognized 'user' parameter; extracting");
			c->username = (*p)->value;
			c->pwhash = pgr_auth_find(c->context, c->username);
			if (c->pwhash == NULL) {
				pgr_logf(stderr, LOG_ERR, "did not find %s user in authdb; authentication *will* fail",
						c->username);
			} else {
				pgr_debugf("found %s user in authdb with pwhash %s",
						c->username, c->pwhash);
			}

		} else if (strcmp((*p)->name, "database") == 0) {
			pgr_debugf("recognized 'database' parameter; extracting");
			c->database = (*p)->value;
		}

		p = &(*p)->next;
	}

	return 0;
}

static int check_auth(CONNECTION *c, MBUF *m)
{
	if (c->pwhash == NULL) {
		return 1;
	}

	MD5 md5;
	char hashed[33];
	memset(hashed, 0, sizeof(hashed));
	pgr_md5_init(&md5);

	pgr_md5_update(&md5, c->pwhash, strlen(c->pwhash));
	pgr_md5_update(&md5, c->salt, 4);
	pgr_md5_hex(hashed, &md5);

	pgr_debugf("checking auth token %s against (calculated) %s",
			pgr_mbuf_data(m, 3, 33), hashed);
	return memcmp(pgr_mbuf_data(m, 3, 32), hashed, 32);
}

static void free_params(PARAM *p)
{
	PARAM *tmp;
	while (p) {
		tmp = p->next;
		free(p->name);
		free(p->value);
		free(p);
		p = tmp;
	}
}

void pgr_conn_init(CONTEXT *c, CONNECTION *dst)
{
	memset(dst, 0, sizeof(CONNECTION));
	dst->context = c;

	dst->serial  = -1;
	dst->index   = -1;
	dst->fd      = -1;

	int rnd = pgr_rand(0, 0xffffffff);
	memcpy(dst->salt, &rnd, 4);
}

void pgr_conn_deinit(CONNECTION *c)
{
	if (c->fd >= 0) {
		close(c->fd);
	}
	free_params(c->params);
}

void pgr_conn_frontend(CONNECTION *dst, int fd)
{
	dst->fd = fd;
}

void pgr_conn_backend(CONNECTION *dst, BACKEND *b, int i)
{
	dst->index = i;
	dst->serial = b->serial;

	dst->hostname = strdup(b->hostname);
	dst->port = b->port;
}

int pgr_conn_copy(CONNECTION *dst, CONNECTION *src)
{
	PARAM *a, *b;

	dst->pwhash = src->pwhash;

	free_params(dst->params);
	a = NULL;
	for (b = src->params; b; b = b->next) {
		if (a) {
			a->next = copy_params(b);
			a = a->next;
		} else {
			dst->params = a = copy_params(b);
		}
	}

	return 0;
}

int pgr_conn_connect(CONNECTION *c)
{
	int rc;
	MBUF *m;

	m = pgr_mbuf_new(512);

	c->fd = pgr_connect(c->hostname, c->port, c->timeout * 1000);
	if (c->fd < 0) {
		return c->fd;
	}

	pgr_mbuf_setfd(m, c->fd, c->fd);

	/* send StartupMessage */
	pgr_debugf("sending StartupMessage to backend (fd %d)", c->fd);
	rc = startup_message(m, c);
	if (rc != 0) {
		return rc;
	}
	rc = pgr_mbuf_relay(m);
	if (rc != 0) {
		return rc;
	}

	/* process replies from remote */
	for (;;) {
		pgr_debugf("waiting for message from backend (fd %d)", c->fd);
		rc = pgr_mbuf_recv(m);
		if (rc != 0) {
			return rc;
		}

		char type = pgr_mbuf_msgtype(m);
		switch (type) {
		case 'E':
			pgr_logf(stderr, LOG_ERR, "received an Error from backend (fd %d)", c->fd);
			return -1;

		case 'N':
			pgr_logf(stderr, LOG_ERR, "received a Notice from backend (fd %d)", c->fd);
			break;

		case 'R': /* Authentication */
			switch (pgr_mbuf_u32(m, 0)) {
			case 0:                                /* AuthenticationOK */
				pgr_mbuf_discard(m);
				break;

			case 5:                       /* AuthenticationMD5Password */
				memcpy(c->salt, pgr_mbuf_data(m, 4, 4), 4);
				pgr_mbuf_discard(m);

				pgr_debugf("sending PasswordMessage to backend (fd %d)", c->fd);
				rc = password_message(m, c);
				if (rc != 0) {
					return rc;
				}
				rc = pgr_mbuf_relay(m);
				if (rc != 0) {
					return rc;
				}
				break;

			default:                           /* all other auth types */
				pgr_logf(stderr, LOG_ERR, "unsupported authentication type %d", pgr_mbuf_u32(m, 0));
				return -1;
			}
			break;

		case 'K': /* BackendKeyData */
			/* FIXME: BackendKeyData is currently ignored */
			pgr_mbuf_discard(m);
			break;

		case 'S': /* ParameterStatus */
			/* FIXME: ParameterStatus is currently ignored */
			pgr_mbuf_discard(m);
			break;

		case 'Z': /* ReadyForQuery */
			pgr_mbuf_discard(m);
			return 0;

		default:
			pgr_debugf("invalid '%c' message received from frontend; disconnecting");
			return -1;
		}
	}
}

int pgr_conn_accept(CONNECTION *c)
{
	int rc;
	char type;
	MBUF *m;

	m = pgr_mbuf_new(512);

	/* receive all messages from client */
	for (;;) {
		pgr_debugf("awaiting message from connection %p (fd %d)", c, c->fd);
		rc = pgr_mbuf_recv(m);
		if (rc != 0) {
			return rc;
		}

		type = pgr_mbuf_msgtype(m);
		switch (type) {
		case MSG_SSLREQ:
			/* FIXME: SSL not supported in this iteration */
			pgr_debugf("received SSLRequest; replying with 'N' (not supported)");
			pgr_mbuf_discard(m);
			if (write(c->fd, "N", 1) != 1) {
				return -1;
			}
			break;

		case MSG_CANCEL:
			pgr_debugf("ignoring CancelRequest (unimplemented)");
			pgr_mbuf_discard(m);
			break;

		case MSG_STARTUP:
			pgr_debugf("extracting parameters from StartupMessage");
			rc = extract_params(c, m);
			if (rc != 0) {
				return rc;
			}
			pgr_mbuf_discard(m);

			pgr_debugf("sending AuthenticationMD5Password to frontend (fd %d)", c->fd);
			rc = auth_md5_message(m, c);
			if (rc != 0) {
				return rc;
			}
			rc = pgr_mbuf_relay(m);
			if (rc != 0) {
				return rc;
			}
			break;

		case 'p': /* PasswordMessage */
			pgr_debugf("received PasswordMessage");
			rc = check_auth(c, m);
			if (rc != 0) {
				error_response(m, "ERROR", "28P01",
						"password authentication failed for user \"%s\"", c->username);
				rc = pgr_mbuf_relay(m);
				if (rc != 0) {
					pgr_logf(stderr, LOG_ERR, "failed to send ErrorResponse to frontend (in response to md5 authentication failure)");
				}
				return 1;
			}

			pgr_debugf("authentication succeeded; sending AuthenticationOk to frontend");
			rc = auth_ok_message(m, c);
			if (rc != 0) {
				return rc;
			}
			rc = pgr_mbuf_relay(m);
			if (rc != 0) {
				return rc;
			}

			/* FIXME: should we defer ReadyForQuery until we connect to reader/writer? */
			pgr_debugf("sending ReadyForQuery to frontend");
			rc = ready_for_query(m, c);
			if (rc != 0) {
				return rc;
			}
			rc = pgr_mbuf_relay(m);
			if (rc != 0) {
				return rc;
			}
			return 0;

		default:
			pgr_debugf("invalid '%c' message received from frontend; disconnecting", type);
			return -1;
		}
	}
}
