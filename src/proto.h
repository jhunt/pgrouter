#ifndef PROTO_H
#define PROTO_H
#include <stdint.h>

/* Messages that don't include a message type byte
   (and therefore are not assigned an ASCII character)
   start at 0x80 (128) and go up.  This should not conflict
   with any official message type assignments. */

#define  PG3_MSG_STARTUP                 0x80  /*  frontend  */
#define  PG3_MSG_SSL_REQUEST             0x81  /*  frontend  */
#define  PG3_MSG_CANCEL_REQUEST          0x82  /*  frontend  */
#define  PG3_MSG_PARSE_COMPLETE          '1'   /*  backend   */
#define  PG3_MSG_BIND_COMPLETE           '2'   /*  backend   */
#define  PG3_MSG_CLOSE_COMPLETE          '3'   /*  backend   */
#define  PG3_MSG_NOTIFICATION_RESPONSE   'A'   /*  backend   */
#define  PG3_MSG_BIND                    'B'   /*  frontend  */
#define  PG3_MSG_COMMAND_COMPLETE        'C'   /*  backend   */
#define  PG3_MSG_COPY_DONE               'c'   /*  BOTH      */
#define  PG3_MSG_CLOSE                   'C'   /*  frontend  */
#define  PG3_MSG_COPY_DATA               'd'   /*  BOTH      */
#define  PG3_MSG_DATA_ROW                'D'   /*  backend   */
#define  PG3_MSG_DESCRIBE                'D'   /*  frontend  */
#define  PG3_MSG_ERROR_RESPONSE          'E'   /*  backend   */
#define  PG3_MSG_EXECUTE                 'E'   /*  frontend  */
#define  PG3_MSG_COPY_FAIL               'f'   /*  frontend  */
#define  PG3_MSG_FUNCTION_CALL           'F'   /*  frontend  */
#define  PG3_MSG_COPY_IN_RESPONSE        'G'   /*  backend   */
#define  PG3_MSG_COPY_OUT_RESPONSE       'H'   /*  backend   */
#define  PG3_MSG_FLUSH                   'H'   /*  frontend  */
#define  PG3_MSG_EMPTY_QUERY_RESPONSE    'I'   /*  backend   */
#define  PG3_MSG_BACKEND_KEY_DATA        'K'   /*  backend   */
#define  PG3_MSG_NO_DATA                 'n'   /*  backend   */
#define  PG3_MSG_NOTICE_RESPONSE         'N'   /*  backend   */
#define  PG3_MSG_PASSWORD_MESSAGE        'p'   /*  frontend  */
#define  PG3_MSG_PARSE                   'P'   /*  frontend  */
#define  PG3_MSG_QUERY                   'Q'   /*  frontend  */
#define  PG3_MSG_AUTH                    'R'   /*  backend   */
#define  PG3_MSG_PORTAL_SUSPENDED        's'   /*  backend   */
#define  PG3_MSG_PARAMETER_STATUS        'S'   /*  backend   */
#define  PG3_MSG_SYNC                    'S'   /*  frontend  */
#define  PG3_MSG_PARAMETER_DESCRIPTION   't'   /*  backend   */
#define  PG3_MSG_ROW_DESCRIPTION         'T'   /*  backend   */
#define  PG3_MSG_FUNCTION_CALL_RESPONSE  'V'   /*  backend   */
#define  PG3_MSG_COPY_BOTH_RESPONSE      'W'   /*  backend   */
#define  PG3_MSG_TERMINATE               'X'   /*  frontend  */
#define  PG3_MSG_READY_FOR_QUERY         'Z'   /*  backend   */

const char* pg3_type_name(int type);

typedef struct {
	uint8_t type;     /* one of the PG3_MSG_* constants      */
	uint32_t length;  /* length of message payload (data+4)  */
	uint8_t *data;    /* the actual data, (len-4) octets     */
} PG3_MSG;

typedef struct {
	char *severity;   /* */
	char *sqlstate;   /* */
	char *message;    /* */
	char *details;    /* */
	char *hint;       /* */
} PG3_ERROR;

/* FIXME: need timeout variation of pg3_recv */
int pg3_recv(int fd, PG3_MSG *msg, int typed);
int pg3_send(int fd, PG3_MSG *msg);
void pf3_free(PG3_MSG *msg);

int pg3_error(PG3_MSG *msg, PG3_ERROR *err);

#endif
