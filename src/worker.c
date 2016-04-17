#include "pgrouter.h"
#include "proto.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#define SUBSYS "worker"
#include "locks.inc.c"

#define max(a,b) ((a) > (b) ? (a) : (b))

#define  FSM_START          0
#define  FSM_SHUTDOWN       1
#define  FSM_STARTED        2
#define  FSM_AUTH_REQUIRED  3
#define  FSM_AUTH_SENT      4
#define  FSM_READY          5
#define  FSM_QUERY_SENT     6
#define  FSM_PARSE_SENT     7
#define  FSM_BIND_READY     8
#define  FSM_BIND_SENT      9
#define  FSM_EXEC_READY     10
#define  FSM_EXEC_SENT      11
#define  FSM_WAIT_SYNC      12
#define  FSM_SYNC_SENT      13

static int connect_to_any_backend(CONTEXT *c, int *i, int *fd)
{
	return 1;
}

static int wait_for_message_from(int fd, PG3_MSG *msg, int type)
{
	return 1;
}

static int send_error_to(int fd, const char *error)
{
	return 1;
}

static int send_msg_to(int fd, PG3_MSG *msg)
{
	return 1;
}

static void handle_client(CONTEXT *c, int fefd)
{
	int state = FSM_START;
	int rc, backend, befd, port;
	char *host;
	PG3_MSG msg;
	PG3_ERROR err;

	for (;;) {
		switch (state) {
		case FSM_START:
			if (!wait_for_message_from(fefd, &msg, PG3_MSG_STARTUP)) {
				state = FSM_SHUTDOWN;
				break;
			}

			if (!connect_to_any_backend(c, &backend, &befd)) {
				send_error_to(fefd, "Unable to connect to a backend");
				state = FSM_SHUTDOWN;
				break;
			}

			state = FSM_STARTED;
			break;

		case FSM_STARTED:
			if (!wait_for_message_from(befd, &msg, 0)) {
				send_error_to(fefd, "Backend connection timed out");
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_ERROR_RESPONSE:
				send_msg_to(fefd, &msg);
				state = FSM_SHUTDOWN;
				break;

			case PG3_MSG_AUTH:
				send_msg_to(fefd, &msg);
				state = FSM_AUTH_REQUIRED; /* FIXME: only on Authentication requests */
				break;

			case PG3_MSG_NOTICE_RESPONSE:
			case PG3_MSG_BACKEND_KEY_DATA:
			case PG3_MSG_PARAMETER_STATUS:
				send_msg_to(fefd, &msg);
				break;

			case PG3_MSG_READY_FOR_QUERY:
				send_msg_to(fefd, &msg);
				state = FSM_READY;
				break;

			default:
				send_error_to(fefd, "Protocol violation");
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_AUTH_REQUIRED:
			if (!wait_for_message_from(fefd, &msg, 0)) {
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_PASSWORD_MESSAGE:
				/* FIXME: save the password message */
				if (!send_msg_to(befd, &msg)) {
					send_error_to(fefd, "Backend communication failure");
					state = FSM_SHUTDOWN;
					break;
				}
				state = FSM_AUTH_SENT;
				break;

			default:
				send_error_to(fefd, "Protocol violation");
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_AUTH_SENT:
			if (!wait_for_message_from(befd, &msg, 0)) {
				send_error_to(fefd, "Backend connection timed out");
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_ERROR_RESPONSE:
				send_msg_to(fefd, &msg);
				state = FSM_SHUTDOWN;
				break;

			case PG3_MSG_AUTH:
				send_msg_to(fefd, &msg);
				state = FSM_STARTED; /* FIXME: only on AuthenticationOk */
				break;

			default:
				send_error_to(fefd, "Protocol violation");
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_READY:
			/* FIXME: wait for message from fefd, or disconnect and exit */
			/* FIXME: on Query: */
			/* FIXME:   inspect query contents to determine if it is RO or RW */
			/* FIXME:   for RO, relay to befd and transition to FSM_QUERY_SENT */
			/* FIXME:   for RW, */
			/* FIXME:     if befd is not the write master: */
			/* FIXME:       disconnect befd from read slave */
			/* FIXME:       connect befd to write master */
			/* FIXME:       send normal StartupMessage */
			/* FIXME:       handle Authentication / Key Data / etc. */
			/* FIXME:     relay to befd and transition to FSM_QUERY_SENT */
			/* FIXME: on Parse: */
			/* FIXME:   inspect query contents to determine if it is RO or RW */
			/* FIXME:   for RO, relay to befd and transition to FSM_PARSE_SENT */
			/* FIXME:   for RW, */
			/* FIXME:     if befd is not the write master: */
			/* FIXME:       disconnect befd from read slave */
			/* FIXME:       connect befd to write master */
			/* FIXME:       send normal StartupMessage */
			/* FIXME:       handle Authentication / Key Data / etc. */
			/* FIXME:     relay to befd and transition to FSM_PARSE_SENT */
			/* FIXME: on *: send ErrorResponse to fefd and exit */
			break;

		case FSM_QUERY_SENT:
			if (!wait_for_message_from(befd, &msg, 0)) {
				send_error_to(fefd, "Backend connection timed out");
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_ERROR_RESPONSE:
				send_msg_to(fefd, &msg);
				state = FSM_SHUTDOWN;
				break;

			case PG3_MSG_NOTICE_RESPONSE:
			case PG3_MSG_COMMAND_COMPLETE:
			case PG3_MSG_ROW_DESCRIPTION:
			case PG3_MSG_DATA_ROW:
			case PG3_MSG_EMPTY_QUERY_RESPONSE:
				send_msg_to(fefd, &msg);
				break;

			case PG3_MSG_COPY_IN_RESPONSE:
			case PG3_MSG_COPY_OUT_RESPONSE:
				/* FIXME: handle copy in / copy out */
				pgr_abort(ABORT_UNIMPL);

			case PG3_MSG_READY_FOR_QUERY:
				send_msg_to(fefd, &msg);
				state = FSM_READY;
				break;

			default:
				send_error_to(fefd, "Protocol violation");
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_PARSE_SENT:
			if (!wait_for_message_from(befd, &msg, 0)) {
				send_error_to(fefd, "Backend connection timed out");
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_ERROR_RESPONSE:
				send_msg_to(fefd, &msg);
				state = FSM_SHUTDOWN;
				break;

			case PG3_MSG_PARSE_COMPLETE:
				send_msg_to(fefd, &msg);
				state = FSM_BIND_READY;
				break;

			default:
				send_error_to(fefd, "Protocol violation");
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_BIND_READY:
			if (!wait_for_message_from(fefd, &msg, 0)) {
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_BIND:
				send_msg_to(befd, &msg);
				state = FSM_BIND_SENT;
				break;

			default:
				send_error_to(fefd, "Protocol violation");
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_BIND_SENT:
			if (!wait_for_message_from(befd, &msg, 0)) {
				send_error_to(fefd, "Backend connection timed out");
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_ERROR_RESPONSE:
				send_msg_to(fefd, &msg);
				state = FSM_SHUTDOWN;
				break;

			case PG3_MSG_BIND_COMPLETE:
				send_msg_to(fefd, &msg);
				state = FSM_EXEC_READY;
				break;

			default:
				send_error_to(fefd, "Protocol violation");
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_EXEC_READY:
			if (!wait_for_message_from(fefd, &msg, 0)) {
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_EXECUTE:
				send_msg_to(befd, &msg);
				state = FSM_EXEC_SENT;
				break;

			default:
				send_error_to(fefd, "Protocol violation");
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_EXEC_SENT:
			if (!wait_for_message_from(befd, &msg, 0)) {
				send_error_to(fefd, "Backend connection timed out");
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_ERROR_RESPONSE:
				send_msg_to(fefd, &msg);
				state = FSM_SHUTDOWN;
				break;

			case PG3_MSG_COMMAND_COMPLETE:
				send_msg_to(fefd, &msg);
				state = FSM_WAIT_SYNC;
				break;

			case PG3_MSG_COPY_IN_RESPONSE:
			case PG3_MSG_COPY_OUT_RESPONSE:
				/* FIXME: handle copy in / copy out */
				pgr_abort(ABORT_UNIMPL);

			case PG3_MSG_DATA_ROW:
			case PG3_MSG_EMPTY_QUERY_RESPONSE:
				send_msg_to(fefd, &msg);
				break;

			default:
				send_error_to(fefd, "Protocol violation");
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_WAIT_SYNC:
			if (!wait_for_message_from(fefd, &msg, 0)) {
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_SYNC:
				send_msg_to(befd, &msg);
				state = FSM_SYNC_SENT;
				break;

			default:
				send_error_to(fefd, "Protocol violation");
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_SYNC_SENT:
			if (!wait_for_message_from(befd, &msg, 0)) {
				send_error_to(fefd, "Backend connection timed out");
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_READY_FOR_QUERY:
				send_msg_to(befd, &msg);
				state = FSM_READY;
				break;

			default:
				send_error_to(fefd, "Protocol violation");
				state = FSM_SHUTDOWN;
				break;
			}
			break;
		}
	}
}

static void* do_worker(void *_c)
{
	CONTEXT *c = (CONTEXT*)_c;
	int rc, connfd, i, nfds;
	int watch[2] = { c->frontend4, c->frontend6 };
	fd_set rfds;

	for (;;) {
		FD_ZERO(&rfds);
		nfds = 0;

		for (i = 0; i < sizeof(watch)/sizeof(watch[0]); i++) {
			if (watch[i] >= 0) {
				FD_SET(watch[i], &rfds);
				nfds = max(watch[i], nfds);
			}
		}

		rc = select(nfds+1, &rfds, NULL, NULL, NULL);
		if (rc == -1) {
			if (errno == EINTR) {
				continue;
			}
			pgr_logf(stderr, LOG_ERR, "[worker] select received system error: %s (errno %d)",
					strerror(errno), errno);
			pgr_abort(ABORT_SYSCALL);
		}

		for (i = 0; i < sizeof(watch)/sizeof(watch[0]); i++) {
			if (watch[i] >= 0 && FD_ISSET(watch[i], &rfds)) {
				connfd = accept(watch[i], NULL, NULL);
				rc = wrlock(&c->lock, "context", 0);
				if (rc != 0) {
					pgr_logf(stderr, LOG_ERR, "[worker] unable to lock context; client connections stats *will* be incorrect");
					handle_client(c, connfd);

				} else {
					c->fe_conns++;
					unlock(&c->lock, "context", 0);
					handle_client(c, connfd);

					rc = wrlock(&c->lock, "context", 0);
					if (rc != 0) {
						pgr_logf(stderr, LOG_ERR, "[worker] unable to lock context; client connections stats *will* be incorrect");
					} else {
						c->fe_conns--;
						unlock(&c->lock, "context", 0);
					}

				}
				close(connfd);
			}
		}
	}

	close(c->frontend4);
	close(c->frontend6);
	return NULL;
}

int pgr_worker(CONTEXT *c, pthread_t *tid)
{
	int rc = pthread_create(tid, NULL, do_worker, c);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[worker] failed to spin up: %s (errno %d)",
				strerror(errno), errno);
		return 1;
	}

	pgr_logf(stderr, LOG_INFO, "[worker] spinning up [tid=%i]", *tid);
	return 0;
}
