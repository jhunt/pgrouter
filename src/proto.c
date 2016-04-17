#include "pgrouter.h"
#include "proto.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int pg3_recv(int fd, PG3_MSG *msg, int type)
{
	errno = EIO;
	int rc;

	if (type == 0) {
		rc = pgr_recvn(fd, &msg->type, sizeof(msg->type));
		if (rc != 0) {
			return rc;
		}
	} else {
		msg->type = type;
	}
	pgr_debugf("message type is %d (%c)",
			msg->type, isprint(msg->type) ? msg->type : '.');

	rc = pgr_recvn(fd, &msg->length, sizeof(msg->length));
	if (rc != 0) {
		return rc;
	}
	msg->length = ntohl(msg->length);
	pgr_debugf("message is %d octets long", msg->length);

	if (msg->length > 65536) {
		pgr_debugf("message is too long; bailing");
		return 3;
	}

	if (msg->length > sizeof(msg->length)) {
		pgr_debugf("allocating a %d byte buffer to store message payload",
				msg->length - sizeof(msg->length));
		msg->data = malloc(msg->length - sizeof(msg->length));
		if (!msg->data) {
			return 4;
		}

		rc = pgr_recvn(fd, msg->data, msg->length - sizeof(msg->length));
		if (rc != 0) {
			free(msg->data);
			return rc;
		}
	}

	return 0;
}

int pg3_send(int fd, PG3_MSG *msg)
{
	int rc;
	uint32_t len;

	if (msg->type < 0x80) {
		rc = pgr_sendn(fd, &msg->type, sizeof(msg->type));
		if (rc != 0) {
			return rc;
		}
	}
	len = htonl(msg->length);
	pgr_debugf("converted %08x (%d) length to network byte order as %08x",
			msg->length, msg->length, len);
	rc = pgr_sendn(fd, &len, sizeof(len));
	if (rc != 0) {
		return rc;
	}

	if (msg->length > 4) {
		rc = pgr_sendn(fd, msg->data, msg->length - 4);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

void pg3_free(PG3_MSG *msg)
{
	free(msg->data);
	msg->data = NULL;
}

int pg3_error(PG3_MSG *msg, PG3_ERROR *err)
{
	msg->length = (1 /* type field */ + strlen(err->severity) + 1 /* null-terminator */)
	            + (1 /* type field */ + strlen(err->sqlstate) + 1 /* null-terminator */)
	            + (1 /* type field */ + strlen(err->message)  + 1 /* null-terminator */)
	            + 4 /* length field */;

	if (err->details != NULL) {
		msg->length += (1 /* type field */ + strlen(err->details)  + 1 /* null-terminator */);
	}
	if (err->hint != NULL) {
		msg->length += (1 /* type field */ + strlen(err->hint)     + 1 /* null-terminator */);
	}

	msg->data = malloc(msg->length);
	if (!msg->data) {
		return -1;
	}

	char *s;
	uint8_t *p = msg->data;

	*p++ = 'S'; for (s = err->severity; *s; *p++ = *s++) ; *p++ = '\0';
	*p++ = 'C'; for (s = err->sqlstate; *s; *p++ = *s++) ; *p++ = '\0';
	*p++ = 'M'; for (s = err->message;  *s; *p++ = *s++) ; *p++ = '\0';
	if (err->details != NULL) {
		*p++ = 'D'; for (s = err->details; *s; *p++ = *s++) ; *p++ = '\0';
	}
	if (err->hint != NULL) {
		*p++ = 'H'; for (s = err->hint; *s; *p++ = *s++) ; *p++ = '\0';
	}
	return 0;
}
