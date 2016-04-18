#include "pgrouter.h"
#include "proto.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static int negative(int x)
{
	return x > 0 ? -1 * x : x;
}

const char* pg3_type_name(int type)
{
	switch (type) {
	case PG3_MSG_STARTUP:                 return "StartupMessage";
	case PG3_MSG_SSL_REQUEST:             return "SSLRequest";
	case PG3_MSG_CANCEL_REQUEST:          return "CancelRequest";
	case PG3_MSG_PARSE_COMPLETE:          return "ParseComplete";
	case PG3_MSG_BIND_COMPLETE:           return "BindComplete";
	case PG3_MSG_CLOSE_COMPLETE:          return "CloseComplete";
	case PG3_MSG_NOTIFICATION_RESPONSE:   return "NotificationResponse";
	case PG3_MSG_BIND:                    return "Bind";
	case PG3_MSG_COMMAND_COMPLETE:        return "CommandComplete/Close";
	case PG3_MSG_COPY_DONE:               return "CopyDone";
	case PG3_MSG_DATA_ROW:                return "DataRow/Describe";
	case PG3_MSG_COPY_DATA:               return "CopyData";
	case PG3_MSG_ERROR_RESPONSE:          return "ErrorResponse/Execute";
	case PG3_MSG_COPY_FAIL:               return "CopyFail";
	case PG3_MSG_FUNCTION_CALL:           return "FunctionCall";
	case PG3_MSG_COPY_IN_RESPONSE:        return "CopyInResponse";
	case PG3_MSG_COPY_OUT_RESPONSE:       return "CopyOutResponse/Flush";
	case PG3_MSG_EMPTY_QUERY_RESPONSE:    return "EmptyQueryResponse";
	case PG3_MSG_BACKEND_KEY_DATA:        return "BackendKeyData";
	case PG3_MSG_NO_DATA:                 return "NoData";
	case PG3_MSG_NOTICE_RESPONSE:         return "NoticeResponse";
	case PG3_MSG_PASSWORD_MESSAGE:        return "PasswordMessage";
	case PG3_MSG_PARSE:                   return "Parse";
	case PG3_MSG_QUERY:                   return "Query";
	case PG3_MSG_AUTH:                    return "Auth";
	case PG3_MSG_PORTAL_SUSPENDED:        return "PortalSuspended";
	case PG3_MSG_PARAMETER_STATUS:        return "ParameterStatus/Sync";
	case PG3_MSG_PARAMETER_DESCRIPTION:   return "ParameterDescription";
	case PG3_MSG_ROW_DESCRIPTION:         return "RowDescription";
	case PG3_MSG_FUNCTION_CALL_RESPONSE:  return "FunctionCallResponse";
	case PG3_MSG_COPY_BOTH_RESPONSE:      return "CopyBothResponse";
	case PG3_MSG_TERMINATE:               return "Terminate";
	case PG3_MSG_READY_FOR_QUERY:         return "ReadyForQuery";
	}
}

int pg3_recv(int fd, PG3_MSG *msg, int typed)
{
	errno = EIO;
	int rc;

	if (typed) {
		pgr_debugf("reading %d byte(s) to get type of message", sizeof(msg->type));
		rc = pgr_recvn(fd, &msg->type, sizeof(msg->type));
		if (rc != 0) {
			return negative(rc);
		}
	}
	pgr_debugf("message type is %d (%c)",
			msg->type, isprint(msg->type) ? msg->type : '.');

	rc = pgr_recvn(fd, &msg->length, sizeof(msg->length));
	if (rc != 0) {
		return negative(rc);
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
			return negative(rc);
		}
	}

	if (!typed) {
		pgr_debugf("determing type from message length (%d) and payload characteristics",
				msg->length);

		if (msg->length >= 8) {
			int hi16 = (msg->data[0] << 8) | msg->data[1];
			int lo16 = (msg->data[2] << 8) | msg->data[3];
			const char *type;

			pgr_debugf("length %d and payload %02x %02x (%d) %02x %02x (%d)",
					msg->length,
					msg->data[0], msg->data[1], hi16,
					msg->data[2], msg->data[3], lo16);

			if (hi16 == 1234 && lo16 == 5679) {
				if (msg->length == 8) {
					msg->type = PG3_MSG_SSL_REQUEST;
					type = "SSLRequest";

				} else if (msg->length == 16) {
					msg->type = PG3_MSG_CANCEL_REQUEST;
					type = "CancelRequest";

				} else {
					pgr_debugf("unrecognized untyped message");
					return 5;
				}
			} else {
				pgr_debugf("treating untyped message as a StartupMessage for protocol v%d.%d",
						hi16, lo16);
				msg->type = PG3_MSG_STARTUP;
				type = "StartupMessage";
			}

			pgr_debugf("setting message type to [%s] (non-standard value %d [%02x])",
					type, msg->type, msg->type);

		} else {
			pgr_debugf("untyped message is too short; bailing");
			return 6;
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
