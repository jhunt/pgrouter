#include "pgrouter.h"
#include "proto.h"
#include <stdlib.h>
#include <string.h>

static int startup_message(PG3_MSG *msg, CONNECTION *c)
{
	PARAM *p;
	int off, n;

	msg->type = PG3_MSG_STARTUP;
	msg->length = 4 + 4 + 1;
	for (p = c->params; p; p = p->next) {
		msg->length += strlen(p->name)  + 1
		             + strlen(p->value) + 1;
	}
	msg->data = calloc(msg->length - 4, sizeof(char));
	if (!msg->data) {
		pgr_abort(ABORT_MEMFAIL);
	}

	msg->data[0] = 0; msg->data[1] = 3; /* major: 3 */
	msg->data[2] = 0; msg->data[3] = 0; /* minor: 0 */
	off = 4;
	for (p = c->params; p; p = p->next) {
		n = strlen(p->name);
		memcpy(msg->data + off, p->name, n+1);
		off += n+1;

		n = strlen(p->value);
		memcpy(msg->data + off, p->value, n+1);
		off += n+1;
	}
	msg->data[off] = '\0';
	return 0;
}

static int auth_md5_message(PG3_MSG *msg, CONNECTION *c)
{
	msg->type = 'R';
	msg->length = 12;
	msg->data = calloc(8, sizeof(char));
	msg->data[3] = 5; /* md5 */
	memcpy(msg->data + 4, c->salt, 4);
	return 0;
}

static int auth_ok_message(PG3_MSG *msg, CONNECTION *c)
{
	msg->type = 'R';
	msg->length = 8;
	msg->data = calloc(4, sizeof(char));
	/* implicit zero-value */
	return 0;
}

static int password_message(PG3_MSG *msg, CONNECTION *c)
{
	int rc, n;
	MD5 md5;

	pgr_md5_init(&md5);
	pgr_md5_update(&md5, c->pwhash, 32);
	pgr_md5_update(&md5, c->salt, 4);

	msg->type = 'p';
	msg->length = 40;
	msg->data = malloc(36);

	memcpy(msg->data, "md5", 3);
	pgr_md5_hex(msg->data + 3, &md5);
	msg->data[35] = '\0';
	return 0;
}

static int ready_for_query(PG3_MSG *msg, CONNECTION *c)
{
	msg->type = 'Z';
	msg->length = 5;
	msg->data = calloc(1, sizeof(char));
	if (!msg->data) {
		pgr_abort(ABORT_MEMFAIL);
	}
	msg->data[0] = 'I'; /* idle, not in a transaction block */
	return 0;
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

static int extract_params(CONNECTION *c, PG3_MSG *msg)
{
	PARAM **p;
	char *x;

	/* find the next insertion point */
	for (p = &c->params; *p; p = &(*p)->next)
		;

	/* process parameter name/value pairs */
	for (x = msg->data + 4; x && *x;) {
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

static int check_auth(CONNECTION *c, PG3_MSG *msg)
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
			msg->data + 3, hashed);
	return memcmp(msg->data + 3, hashed, 32);
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

	a = dst->params;
	while (a) {
		b = a->next;
		free(a->name);
		free(a->value);
		free(a);
		a = b;
	}

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
	PG3_MSG msg;

	c->fd = pgr_connect(c->hostname, c->port, 20); /* FIXME: timeout. for realz. */
	if (c->fd < 0) {
		return c->fd;
	}

	/* send StartupMessage */
	pgr_debugf("sending StartupMessage to backend (fd %d)", c->fd);
	rc = startup_message(&msg, c);
	if (rc != 0) {
		return rc;
	}
	rc = pg3_send(c->fd, &msg);
	if (rc != 0) {
		return rc;
	}

	/* process replies from remote */
	for (;;) {
			pgr_debugf("waiting for message from backend (fd %d)", c->fd);
		rc = pg3_recv(c->fd, &msg, 1);
		if (rc != 0) {
			return rc;
		}

		switch (msg.type) {
		case 'E':
			/* FIXME: do something with the error */
			return -1;

		case 'N':
			/* FIXME: do something with the notice */
			break;

		case 'R': /* Authentication */
			switch (msg.auth_code) {
			case 0:                                /* AuthenticationOK */
				break;

			case 5:                       /* AuthenticationMD5Password */
				memcpy(c->salt, msg.data + 4, 4);
				pgr_debugf("sending PasswordMessage to backend (fd %d)", c->fd);
				rc = password_message(&msg, c);
				if (rc != 0) {
					return rc;
				}
				rc = pg3_send(c->fd, &msg);
				if (rc != 0) {
					return rc;
				}
				break;

			default:                           /* all other auth types */
				/* FIXME: error on unsupported auth request! */
				return -1;
			}
			break;

		case 'K': /* BackendKeyData */
			/* FIXME: BackendKeyData is currently ignored */
			break;

		case 'S': /* ParameterStatus */
			/* FIXME: ParameterStatus is currently ignored */
			break;

		case 'Z': /* ReadyForQuery */
			return 0;

		default:
			pgr_debugf("invalid '%c' message received from frontend; disconnecting");
			return -1;
		}
	}
}

int pgr_conn_accept(CONNECTION *c)
{
	int rc, typed;
	PG3_MSG msg;
	PG3_ERROR err;
	memset(&err, 0, sizeof(err));

	/* receive all messages from client */
	typed = 0;
	for (;;) {
		pgr_debugf("awaiting message from connection %p (fd %d)", c, c->fd);
		rc = pg3_recv(c->fd, &msg, typed);
		if (rc != 0) {
			return rc;
		}

		switch (msg.type) {
		case PG3_MSG_SSL_REQUEST:
			/* FIXME: SSL not supported in this iteration */
			pgr_debugf("received SSLRequest; replying with 'N' (not supported)");
			if (write(c->fd, "N", 1) != 1) {
				return -1;
			}
			break;

		case PG3_MSG_STARTUP:
			pgr_debugf("extracting parameters from StartupMessage");
			rc = extract_params(c, &msg);
			if (rc != 0) {
				return rc;
			}
			pgr_debugf("sending AuthenticationMD5Password to backend (fd %d)", c->fd);
			rc = auth_md5_message(&msg, c);
			if (rc != 0) {
				return rc;
			}
			rc = pg3_send(c->fd, &msg);
			if (rc != 0) {
				return rc;
			}
			typed = 1;
			break;

		case 'p': /* PasswordMessage */
			pgr_debugf("received PasswordMessage");
			rc = check_auth(c, &msg);
			if (rc != 0) {
				err.severity = "ERROR";
				err.sqlstate = "28P01";
				asprintf(&err.message, "password authentication failed for user \"%s\"",
						c->username);
				pg3_error(&msg, &err);

				pgr_debugf("authentication failed; sending ErrorResponse %s %s",
						err.severity, err.sqlstate);
				rc = pg3_send(c->fd, &msg);
				if (rc != 0) {
					pgr_logf(stderr, LOG_ERR, "failed to send ErrorResponse to frontend (in response to md5 authentication failure)");
				}
				free(err.message);
				return 1;
			}

			pgr_debugf("authentication succeeded; sending AuthenticationOk to frontend");
			rc = auth_ok_message(&msg, c);
			if (rc != 0) {
				return rc;
			}
			rc = pg3_send(c->fd, &msg);
			if (rc != 0) {
				return rc;
			}

			/* FIXME: should we defer ReadyForQuery until we connect to reader/writer? */
			pgr_debugf("sending ReadyForQuery to frontend");
			rc = ready_for_query(&msg, c);
			if (rc != 0) {
				return rc;
			}
			rc = pg3_send(c->fd, &msg);
			if (rc != 0) {
				return rc;
			}
			return 0;

		default:
			pgr_debugf("invalid '%c' message received from frontend; disconnecting");
			return -1;
		}
	}
}
