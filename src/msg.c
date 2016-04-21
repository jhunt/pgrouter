#include "pgrouter.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static int negative(int x)
{
	return x > 0 ? -1 * x : x;
}

int pgr_msg_recv(int fd, MESSAGE *m)
{
	errno = EIO;
	int rc;

	rc = pgr_recvn(fd, &m->type, sizeof(m->type));
	if (rc != 0) {
		return negative(rc);
	}
	if (isuntyped(m->type)) {
		/* legacy 'untyped' message; first 4 octets are length */
		char size[4] = { m->type, 0, 0, 0 };
		rc = pgr_recvn(fd, size+1, 3);
		if (rc != 0) {
			return negative(rc);
		}
		memcpy(&m->length, size, 4);

	} else {
		rc = pgr_recvn(fd, &m->length, 4);
		if (rc != 0) {
			return negative(rc);
		}
	}

	m->length = ntohl(m->length);
	if (m->length > 65536) {
		pgr_debugf("message of %d (%#02x) octets is too long; bailing",
				m->length, m->length);
		return 3;
	}

	if (m->length > sizeof(m->length)) {
		m->data = malloc(m->length - sizeof(m->length));
		if (!m->data) {
			return 4;
		}

		rc = pgr_recvn(fd, m->data, m->length - sizeof(m->length));
		if (rc != 0) {
			free(m->data);
			return negative(rc);
		}
	}

	if (isuntyped(m->type)) {
		pgr_debugf("determing type from message length (%d) and payload characteristics",
				m->length);

		if (m->length >= 8) {
			unsigned short hi16 = (m->data[0] << 8) | m->data[1];
			unsigned short lo16 = (m->data[2] << 8) | m->data[3];
			const char *type;

			pgr_debugf("length %d and payload %02x %02x (%u) %02x %02x (%u)",
					m->length,
					m->data[0], m->data[1], hi16,
					m->data[2], m->data[3], lo16);

			if (hi16 == 1234 && lo16 == 5679) {
				if (m->length == 8) {
					m->type = MSG_SSL_REQUEST;
					type = "SSLRequest";

				} else if (m->length == 16) {
					m->type = MSG_CANCEL_REQUEST;
					type = "CancelRequest";

				} else {
					pgr_debugf("unrecognized untyped message");
					return 5;
				}
			} else {
				pgr_debugf("treating untyped message as a StartupMessage for protocol v%d.%d",
						hi16, lo16);
				m->type = MSG_STARTUP_MESSAGE;
				type = "StartupMessage";
			}

			pgr_debugf("setting message type to [%s] (non-standard value %d [%02x])",
					type, m->type, m->type);

		} else {
			pgr_debugf("untyped message is too short; bailing");
			return 6;
		}
	}

	/* auxiliary data */
	if (m->type == 'R') {
		m->auth.code = ntohl(*(int*)(m->data));

	} else if (m->type == 'E') {
		char **sub, *x;
		for (sub = NULL, x = m->data; x && *x; ) {
			switch (*x) {
			case 'S': sub = &m->error.severity; break;
			case 'C': sub = &m->error.sqlstate; break;
			case 'M': sub = &m->error.message;  break;
			case 'D': sub = &m->error.details;  break;
			case 'H': sub = &m->error.hint;     break;
			default:  sub = NULL; break;
			}

			x++;
			if (sub) {
				*sub = x;
			}
			x += strlen(x) + 1;
		}
	}

	char *buf;
	int len = htonl(m->length);

	if (isuntyped(m->type)) {
		buf = malloc(m->length);
		memcpy(buf, &len, 4);
		memcpy(buf+4, m->data, m->length - 4);

	} else {
		buf = malloc(1 + m->length);
		buf[0] = m->type;
		memcpy(buf+1, &len, 4);
		memcpy(buf+5, m->data, m->length - 4);
	}
	pgr_hexdump(buf, m->length + (isuntyped(m->type) ? 0 : 1));

	return 0;
}

int pgr_msg_send(int fd, MESSAGE *m)
{
	char *buf;
	int len = htonl(m->length);

	if (isuntyped(m->type)) {
		buf = malloc(m->length);
		memcpy(buf, &len, 4);
		memcpy(buf + 4, m->data, m->length - 4);
		return pgr_sendn(fd, buf, m->length);

	} else {
		buf = malloc(1 + m->length);
		buf[0] = m->type;
		memcpy(buf + 1, &len, 4);
		memcpy(buf + 5, m->data, m->length - 4);
		return pgr_sendn(fd, buf, 1 + m->length);
	}
}

void pgr_msg_clear(MESSAGE *m)
{
	free(m->data);
	memset(m, 0, sizeof(MESSAGE));
}

void pgr_msg_pack(MESSAGE *m)
{
	if (m->type == 'E') { /* ErrorResponse */
		m->length = (1 /* type field */ + strlen(m->error.severity) + 1 /* null-terminator */)
		          + (1 /* type field */ + strlen(m->error.sqlstate) + 1 /* null-terminator */)
		          + (1 /* type field */ + strlen(m->error.message)  + 1 /* null-terminator */)
		          + 4 /* length field */
		          + 1 /* final null-terminator */;

		if (m->error.details != NULL) {
			m->length += (1 /* type field */ + strlen(m->error.details)  + 1 /* null-terminator */);
		}
		if (m->error.hint != NULL) {
			m->length += (1 /* type field */ + strlen(m->error.hint)     + 1 /* null-terminator */);
		}

		m->data = malloc(m->length);
		if (!m->data) {
			pgr_abort(ABORT_MEMFAIL);
		}

		char *s;
		char *p = m->data;

		*p++ = 'S'; for (s = m->error.severity; *s; *p++ = *s++) ; *p++ = '\0';
		*p++ = 'C'; for (s = m->error.sqlstate; *s; *p++ = *s++) ; *p++ = '\0';
		*p++ = 'M'; for (s = m->error.message;  *s; *p++ = *s++) ; *p++ = '\0';
		if (m->error.details != NULL) {
			*p++ = 'D'; for (s = m->error.details; *s; *p++ = *s++) ; *p++ = '\0';
		}
		if (m->error.hint != NULL) {
			*p++ = 'H'; for (s = m->error.hint; *s; *p++ = *s++) ; *p++ = '\0';
		}
		*p = '\0';
	}
}
