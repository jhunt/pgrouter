#include "proto.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int pg3_recv(int fd, PG3_MSG *msg, int type)
{
	size_t nread;

	if (type == 0) {
		if (read(fd, &msg->type, sizeof(msg->type)) != sizeof(msg->type)) {
			return -1;
		}
	} else {
		msg->type = type;
	}
	if (read(fd, &msg->length, sizeof(msg->length)) != sizeof(msg->type)) {
		return -1;
	}

	msg->data = malloc(msg->length);
	if (!msg->data) {
		return -1;
	}

	uint8_t *p = msg->data;
	size_t nleft = msg->length;
	while (nleft > 0) {
		nread = read(fd, p, nleft);
		if (nread < 0) {
			free(msg->data);
			msg->data = NULL;
			return -1;
		}

		nleft -= nread;
		p += nread;
	}

	return 0;
}

int pg3_send(int fd, PG3_MSG *msg)
{
	/* FIXME: handle legacy, non-typed messages */
	size_t nwrit;

	if (write(fd, &msg->type, sizeof(msg->type)) != sizeof(msg->type)) {
		return -1;
	}
	uint32_t len = msg->length;
	htonl(len);
	if (write(fd, &len, sizeof(len)) != sizeof(len)) {
		return -1;
	}

	len = msg->length;
	uint8_t *p;
	while (len > 0) {
		nwrit = write(fd, p, len);
		if (nwrit <= 0) {
			return -1;
		}
		len -= nwrit;
		p += nwrit;
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
	            + (1 /* type field */ + strlen(err->details)  + 1 /* null-terminator */)
	            + (1 /* type field */ + strlen(err->hint)     + 1 /* null-terminator */)
	            + 4 /* length field */;
	msg->data = malloc(msg->length);
	if (!msg->data) {
		return -1;
	}

	char *s;
	uint8_t *p = msg->data;

	*p++ = 'S'; for (s = err->severity; *s; *p++ = *s++) ; *p++ = '\0';
	*p++ = 'C'; for (s = err->sqlstate; *s; *p++ = *s++) ; *p++ = '\0';
	*p++ = 'M'; for (s = err->message;  *s; *p++ = *s++) ; *p++ = '\0';
	*p++ = 'D'; for (s = err->details;  *s; *p++ = *s++) ; *p++ = '\0';
	*p++ = 'H'; for (s = err->hint;     *s; *p++ = *s++) ; *p++ = '\0';
	return 0;
}
