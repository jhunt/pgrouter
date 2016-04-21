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
		rc = pgr_recvn(fd, &msg->type, sizeof(msg->type));
		if (rc != 0) {
			return negative(rc);
		}
	}

	rc = pgr_recvn(fd, &msg->length, sizeof(msg->length));
	if (rc != 0) {
		return negative(rc);
	}

	msg->length = ntohl(msg->length);
	if (msg->length > 65536) {
		pgr_debugf("message of %d (%#02x) octets is too long; bailing",
				msg->length, msg->length);
		return 3;
	}

	if (msg->length > sizeof(msg->length)) {
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

	/* auxiliary data */
	if (msg->type == 'R') {
		msg->auth_code = ntohl(*(int*)(msg->data));
	}

	uint8_t *buf;
	uint32_t len = htonl(msg->length);

	if (typed) {
		buf = malloc(1 + msg->length);
		buf[0] = msg->type;
		memcpy(buf+1, &len, 4);
		memcpy(buf+5, msg->data, msg->length - 4);

	} else {
		buf = malloc(msg->length);
		memcpy(buf, &len, 4);
		memcpy(buf+4, msg->data, msg->length - 4);
	}
	pgr_hexdump(buf, msg->length + (typed ? 1 : 0));

	return 0;
}

int pg3_send(int fd, PG3_MSG *msg)
{
	uint8_t *buf;
	uint32_t len = htonl(msg->length);

	if (msg->type < 0x80) {
		buf = malloc(1 + msg->length);
		buf[0] = msg->type;
		memcpy(buf + 1, &len, 4);
		memcpy(buf + 5, msg->data, msg->length - 4);
		return pgr_sendn(fd, buf, 1 + msg->length);

	} else {
		buf = malloc(msg->length);
		memcpy(buf, &len, 4);
		memcpy(buf + 4, msg->data, msg->length - 4);
		return pgr_sendn(fd, buf, msg->length);
	}
}

void pg3_free(PG3_MSG *msg)
{
	free(msg->data);
	msg->data = NULL;
}

int pg3_send_authmd5(int fd, char salt[4])
{
	/* this one is simple enough, just build the buffer */
	int rc;
	uint8_t buf[1 + 4 + 8];
	memset(&buf, 0, sizeof(buf));
	buf[0] = PG3_MSG_AUTH;
	buf[4] = 12; /* length = 12 */
	buf[8] = 5;  /* value 5 == md5 */
	memcpy(buf + 9, salt, 4);

	return pgr_sendn(fd, buf, sizeof(buf));
}

int pg3_send_password(int fd, const char *crypt)
{
}

void pg3_error(PG3_MSG *msg, PG3_ERROR *err)
{
	msg->type = 'E';
	msg->length = (1 /* type field */ + strlen(err->severity) + 1 /* null-terminator */)
	            + (1 /* type field */ + strlen(err->sqlstate) + 1 /* null-terminator */)
	            + (1 /* type field */ + strlen(err->message)  + 1 /* null-terminator */)
	            + 4 /* length field */
	            + 1 /* final null-terminator */;

	if (err->details != NULL) {
		msg->length += (1 /* type field */ + strlen(err->details)  + 1 /* null-terminator */);
	}
	if (err->hint != NULL) {
		msg->length += (1 /* type field */ + strlen(err->hint)     + 1 /* null-terminator */);
	}

	msg->data = malloc(msg->length);
	if (!msg->data) {
		pgr_abort(ABORT_MEMFAIL);
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
	*p = '\0';
}
