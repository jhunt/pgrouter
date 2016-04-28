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

static int startup_message(MSG *m, CONNECTION *c)
{
	PARAM *p;
	unsigned int len, n, off;

	len = 4 + 4 + 1;
	for (p = c->params; p; p = p->next) {
		len = len + strlen(p->name)   + 1
		          +  strlen(p->value) + 1;
	}

	len = htonl(len);
	pgr_m_write(m, &len, 4);
	pgr_m_write(m, "\0\x3\0\0", 4);

	for (p = c->params; p; p = p->next) {
		pgr_m_write(m, p->name, strlen(p->name)+1);
		pgr_m_write(m, p->value, strlen(p->value)+1);
	}
	pgr_m_write(m, "\0", 1);
	return 0;
}

static int auth_md5_message(MSG *m, CONNECTION *c)
{
	char buf[13];
	memcpy(buf, "R\0\0\0\xc\0\0\0\x5", 9);
	memcpy(buf+9, c->salt, 4);
	return pgr_m_write(m, buf, sizeof(buf));
}

static int auth_ok_message(MSG *m, CONNECTION *c)
{
	return pgr_m_write(m, "R\0\0\0\x8\0\0\0\0", 9);
}

static int password_message(MSG *m, CONNECTION *c)
{
	MD5 md5;
	char buf[41];

	pgr_md5_init(&md5);
	pgr_md5_update(&md5, c->pwhash, 32);
	pgr_md5_update(&md5, c->salt, 4);

	memcpy(buf, "p\0\0\0\x28md5", 8);
	pgr_md5_hex(buf+8, &md5);
	buf[40] = '\0';
	return pgr_m_write(m, buf, sizeof(buf));
}

static int ready_for_query(MSG *m, CONNECTION *c)
{
	return pgr_m_write(m, "Z\0\0\0\x5I", 6);
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

static int extract_params(CONNECTION *c, MSG *m)
{
	PARAM **p;
	char *x;

	/* find the next insertion point */
	for (p = &c->params; *p; p = &(*p)->next)
		;

	/* process parameter name/value pairs */
	for (x = pgr_m_str_at(m, 4+4); x && *x;) {
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

static int check_auth(CONNECTION *c, MSG *m)
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
			pgr_m_str_at(m, 9), hashed);
	return memcmp(pgr_m_str_at(m, 8), hashed, 32);
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
	MSG *rx = pgr_m_new();
	MSG *tx = pgr_m_new();

	c->fd = pgr_connect(c->hostname, c->port, c->timeout * 1000);
	if (c->fd < 0) {
		return c->fd;
	}

	/* send StartupMessage */
	pgr_debugf("sending StartupMessage to backend (fd %d)", c->fd);
	rc = startup_message(tx, c);
	if (rc != 0) {
		return rc;
	}
	rc = pgr_m_send(tx, c->fd);
	pgr_m_flush(tx);
	if (rc != 0) {
		return rc;
	}

	/* process replies from remote */
	for (;;) {
		pgr_debugf("waiting for message from backend (fd %d)", c->fd);
		rc = pgr_m_next(rx, c->fd);
		if (rc != 0) {
			return rc;
		}

		char type = pgr_m_u8_at(rx, 0);
		switch (type) {
		case 'E':
			pgr_logf(stderr, LOG_ERR, "received an Error from backend (fd %d)", c->fd);
			return -1;

		case 'N':
			pgr_logf(stderr, LOG_ERR, "received a Notice from backend (fd %d)", c->fd);
			break;

		case 'R': /* Authentication */
			switch (pgr_m_u32_at(rx, 5)) {
			case 0:                                /* AuthenticationOK */
				pgr_m_discard(rx, c->fd);
				break;

			case 5:                       /* AuthenticationMD5Password */
				memcpy(c->salt, pgr_m_str_at(rx, 9), 4);
				pgr_m_discard(rx, c->fd);

				pgr_debugf("sending PasswordMessage to backend (fd %d)", c->fd);
				rc = password_message(tx, c);
				if (rc != 0) {
					return rc;
				}
				rc = pgr_m_send(tx, c->fd);
				pgr_m_flush(tx);
				if (rc != 0) {
					return rc;
				}
				break;

			default:                           /* all other auth types */
				pgr_logf(stderr, LOG_ERR, "unsupported authentication type %d", pgr_m_u32_at(rx, 5));
				return -1;
			}
			break;

		case 'K': /* BackendKeyData */
			/* FIXME: BackendKeyData is currently ignored */
			pgr_m_discard(rx, c->fd);
			break;

		case 'S': /* ParameterStatus */
			/* FIXME: ParameterStatus is currently ignored */
			pgr_m_discard(rx, c->fd);
			break;

		case 'Z': /* ReadyForQuery */
			pgr_m_discard(rx, c->fd);
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
	MSG *rx = pgr_m_new();
	MSG *tx = pgr_m_new();

	/* receive all messages from client */
	for (;;) {
		pgr_debugf("awaiting message from connection %p (fd %d)", c, c->fd);
		rc = pgr_m_next(rx, c->fd);
		if (rc != 0) {
			return rc;
		}

		type = pgr_m_u8_at(rx, 0);
		switch (type) {
		case 0: /* an untyped message */
			if (pgr_m_u32_at(rx, 0) == 8
			 && pgr_m_u16_at(rx, 4) == 1234
			 && pgr_m_u16_at(rx, 6) == 5679) {
				/* FIXME: SSL not supported in this iteration */
				pgr_debugf("received SSLRequest; replying with 'N' (not supported)");
				pgr_m_discard(rx, c->fd);
				if (write(c->fd, "N", 1) != 1) {
					return -1;
				}

			} else if (pgr_m_u32_at(rx, 0) == 16) {
				pgr_debugf("ignoring CancelRequest (unimplemented)");
				pgr_m_discard(rx, c->fd);

			} else if (pgr_m_u32_at(rx, 0) >= 9) {
				pgr_debugf("extracting parameters from StartupMessage");
				rc = extract_params(c, rx);
				if (rc != 0) {
					return rc;
				}
				pgr_m_discard(rx, c->fd);

				pgr_debugf("sending AuthenticationMD5Password to frontend (fd %d)", c->fd);
				rc = auth_md5_message(tx, c);
				if (rc != 0) {
					return rc;
				}
				rc = pgr_m_send(tx, c->fd);
				pgr_m_flush(tx);
				if (rc != 0) {
					return rc;
				}

			} else {
				pgr_debugf("untyped message is too short");
			}
			break;

		case 'p': /* PasswordMessage */
			pgr_debugf("received PasswordMessage");
			rc = check_auth(c, rx);
			if (rc != 0) {
				pgr_m_errorf(tx, "ERROR", "28P01",
						"password authentication failed for user \"%s\"", c->username);
				rc = pgr_m_send(tx, c->fd);
				if (rc != 0) {
					pgr_logf(stderr, LOG_ERR, "failed to send ErrorResponse to frontend (in response to md5 authentication failure)");
				}
				pgr_m_flush(tx);
				return 1;
			}

			pgr_debugf("authentication succeeded; sending AuthenticationOk to frontend");
			rc = auth_ok_message(tx, c);
			if (rc != 0) {
				return rc;
			}
			rc = pgr_m_send(tx, c->fd);
			pgr_m_flush(tx);
			if (rc != 0) {
				return rc;
			}

			/* FIXME: should we defer ReadyForQuery until we connect to reader/writer? */
			pgr_debugf("sending ReadyForQuery to frontend");
			rc = ready_for_query(tx, c);
			if (rc != 0) {
				return rc;
			}
			rc = pgr_m_send(tx, c->fd);
			pgr_m_flush(tx);
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
