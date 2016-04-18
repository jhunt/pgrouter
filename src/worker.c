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

static char* fsm_name(int n)
{
	switch (n) {
	case FSM_START:         return "START";
	case FSM_SHUTDOWN:      return "SHUTDOWN";
	case FSM_STARTED:       return "STARTED";
	case FSM_AUTH_REQUIRED: return "AUTH_REQUIRED";
	case FSM_AUTH_SENT:     return "AUTH_SENT";
	case FSM_READY:         return "READY";
	case FSM_QUERY_SENT:    return "QUERY_SENT";
	case FSM_PARSE_SENT:    return "PARSE_SENT";
	case FSM_BIND_READY:    return "BIND_READY";
	case FSM_BIND_SENT:     return "BIND_SENT";
	case FSM_EXEC_READY:    return "EXEC_READY";
	case FSM_EXEC_SENT:     return "EXEC_SENT";
	case FSM_WAIT_SYNC:     return "WAIT_SYNC";
	case FSM_SYNC_SENT:     return "SYNC_SENT";
	default:                return "(unknow)";
	}
}

#define  ERR_BADPROTO  1
#define  ERR_CONNFAIL  2
#define  ERR_BEFAIL    3

static int connect_to_any_backend(CONTEXT *c, int *i, int *fd)
{
	int rc;

	rc = pgr_pick_any(c);
	if (rc < 0) {
		return 0;
	}
	*i = rc; /* index into backends */

	rc = rdlock(&c->lock, "context", 0);
	if (rc != 0) {
		return 0;
	}
	rc = rdlock(&c->backends[*i].lock, "backend", *i);
	if (rc != 0) {
		unlock(&c->lock, "context", 0);
		return 0;
	}

	char *host = strdup(c->backends[*i].hostname);
	int port = c->backends[*i].port;

	unlock(&c->backends[*i].lock, "backend", *i);
	unlock(&c->lock, "context", 0);

	rc = pgr_connect(host, port, 30); /* FIXME: timeout not honored; pick a better one! */
	if (rc < 0) {
		free(host);
		return 0;
	}
	free(host);
	*fd = rc;
	return 1;
}

static int wait_for_msg_from(int fd, PG3_MSG *msg, int typed)
{
	pgr_debugf("waiting to receive a message from fd %d", fd);
	/* FIXME: need a timeout version of pg3_recv */
	int rc = pg3_recv(fd, msg, typed);
	if (rc == 0) {
		pgr_debugf("received a [%s] message (%02x / %c)",
				pg3_type_name(msg->type), msg->type, msg->type);
		return 1;
	}
	return 0;
}

static int send_error_to(int fd, int error)
{
	int rc;
	PG3_MSG msg;
	PG3_ERROR err;
	memset(&msg, 0, sizeof(msg));
	memset(&err, 0, sizeof(err));

	switch (error) {
	case ERR_BADPROTO:
		err.severity = "FATAL";
		err.sqlstate = "08P01";
		err.message  = "Protocol violation";
		err.details  = "pgrouter encountered an invalid or inexpected protocol message";
		break;

	case ERR_CONNFAIL:
		err.severity = "FATAL";
		err.sqlstate = "08006";
		err.message  = "Backend connection failed";
		err.details  = "pgrouter was unable to connect to the desired backend database cluster";
		err.hint     = "Check the health of your pgrouter backends";
		break;

	case ERR_BEFAIL:
		err.severity = "FATAL";
		err.sqlstate = "08000";
		err.message  = "Failed to communicate to backend";
		err.details  = "pgrouter was unable to communicate with the connected backend database cluster";
		err.hint     = "Check the health of your pgrouter backends, and consult their logs";
		break;

	default:
		err.severity = "PANIC";
		err.sqlstate = "08000";
		err.message  = "An unrecognized error has occurred";
		err.details  = "pgrouter generated an error condition, but the type of error was not recognized.";
		err.hint     = "Please file a bug report; this is an internal pgrouter error";
		break;
	}

	pgr_debugf("sending error %d to fd %d: %s (%s) %s",
			error, fd, err.severity, err.sqlstate, err.message);

	rc = pg3_error(&msg, &err);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "unable to send ErrorResponse to fd %d: %s (errno %d)",
				fd, strerror(errno), errno);
		return 0;
	}

	rc = pg3_send(fd, &msg);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "unable to send ErrorResponse to fd %d: %s (errno %d)",
				fd, strerror(errno), errno);
		return 0;
	}

	return 1;
}

static int relay_msg_to(int fd, PG3_MSG *msg)
{
	int rc;

	rc = pg3_send(fd, msg);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "unable to relay message to fd %d: %s (errno %d)",
				fd, strerror(errno), errno);
		return 0;
	}
	return 1;
}

static void handle_client(CONTEXT *c, int fefd)
{
	int state = FSM_START;
	int rc, backend, befd, port;
	char *host;
	PG3_MSG msg;

	befd = backend = -1;
	pgr_debugf("not connected to a backend; fefd = %d, befd = %d",
			backend, fefd, befd);

	for (;;) {
		pgr_debugf("FSM[%s] backend = %d, fefd = %d, befd = %d",
				fsm_name(state), backend, fefd, befd);
		switch (state) {
		case FSM_START:
			if (!wait_for_msg_from(fefd, &msg, 0)) {
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_STARTUP:
				if (!connect_to_any_backend(c, &backend, &befd)) {
					send_error_to(fefd, ERR_CONNFAIL);
					state = FSM_SHUTDOWN;
					break;
				}
				pgr_debugf("connected to backend/%d; fefd = %d, befd = %d",
						backend, fefd, befd);

				if (!relay_msg_to(befd, &msg)) {
					state = FSM_SHUTDOWN;
					break;
				}
				state = FSM_STARTED;
				break;

			case PG3_MSG_CANCEL_REQUEST:
				pgr_debugf("CancelRequest not implemented!");
				pgr_abort(ABORT_UNIMPL);
				break;

			case PG3_MSG_SSL_REQUEST:
				if (write(fefd, "N", 1) != 1) {
					pgr_debugf("failed to send SSL response to fd %d: %s (errno %d)",
							fefd, strerror(errno), errno);
					state = FSM_SHUTDOWN;
					break;
				}
				break;

			default:
				pgr_logf(stderr, LOG_ERR, "received unrecognized message type %d (%c); disconnecting",
						msg.type, isprint(msg.type) ? msg.type : '.');
				state = FSM_SHUTDOWN;
				break;
			}

			break;

		case FSM_SHUTDOWN:
			if (befd >= 0) {
				close(befd);
			}
			close(fefd);
			return;

		case FSM_STARTED:
			if (!wait_for_msg_from(befd, &msg, 1)) {
				send_error_to(fefd, ERR_BEFAIL);
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_ERROR_RESPONSE:
				relay_msg_to(fefd, &msg);
				state = FSM_SHUTDOWN;
				break;

			case PG3_MSG_AUTH:
				relay_msg_to(fefd, &msg);
				state = FSM_AUTH_REQUIRED; /* FIXME: only on Authentication requests */
				break;

			case PG3_MSG_NOTICE_RESPONSE:
			case PG3_MSG_BACKEND_KEY_DATA:
			case PG3_MSG_PARAMETER_STATUS:
				relay_msg_to(fefd, &msg);
				break;

			case PG3_MSG_READY_FOR_QUERY:
				relay_msg_to(fefd, &msg);
				state = FSM_READY;
				break;

			default:
				send_error_to(fefd, ERR_BADPROTO);
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_AUTH_REQUIRED:
			if (!wait_for_msg_from(fefd, &msg, 1)) {
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_PASSWORD_MESSAGE:
				/* FIXME: save the password message */
				if (!relay_msg_to(befd, &msg)) {
					send_error_to(fefd, ERR_BEFAIL);
					state = FSM_SHUTDOWN;
					break;
				}
				state = FSM_AUTH_SENT;
				break;

			default:
				send_error_to(fefd, ERR_BADPROTO);
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_AUTH_SENT:
			if (!wait_for_msg_from(befd, &msg, 1)) {
				send_error_to(fefd, ERR_BEFAIL);
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_ERROR_RESPONSE:
				relay_msg_to(fefd, &msg);
				state = FSM_SHUTDOWN;
				break;

			case PG3_MSG_AUTH:
				relay_msg_to(fefd, &msg);
				state = FSM_STARTED; /* FIXME: only on AuthenticationOk */
				break;

			default:
				send_error_to(fefd, ERR_BADPROTO);
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_READY:
			if (!wait_for_msg_from(fefd, &msg, 1)) {
				state = FSM_SHUTDOWN;
				break;
			}

			pgr_abort(ABORT_UNIMPL);
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
			if (!wait_for_msg_from(befd, &msg, 1)) {
				send_error_to(fefd, ERR_BEFAIL);
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_ERROR_RESPONSE:
				relay_msg_to(fefd, &msg);
				state = FSM_SHUTDOWN;
				break;

			case PG3_MSG_NOTICE_RESPONSE:
			case PG3_MSG_COMMAND_COMPLETE:
			case PG3_MSG_ROW_DESCRIPTION:
			case PG3_MSG_DATA_ROW:
			case PG3_MSG_EMPTY_QUERY_RESPONSE:
				relay_msg_to(fefd, &msg);
				break;

			case PG3_MSG_COPY_IN_RESPONSE:
			case PG3_MSG_COPY_OUT_RESPONSE:
				/* FIXME: handle copy in / copy out */
				pgr_abort(ABORT_UNIMPL);

			case PG3_MSG_READY_FOR_QUERY:
				relay_msg_to(fefd, &msg);
				state = FSM_READY;
				break;

			default:
				send_error_to(fefd, ERR_BADPROTO);
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_PARSE_SENT:
			if (!wait_for_msg_from(befd, &msg, 1)) {
				send_error_to(fefd, ERR_BEFAIL);
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_ERROR_RESPONSE:
				relay_msg_to(fefd, &msg);
				state = FSM_SHUTDOWN;
				break;

			case PG3_MSG_PARSE_COMPLETE:
				relay_msg_to(fefd, &msg);
				state = FSM_BIND_READY;
				break;

			default:
				send_error_to(fefd, ERR_BADPROTO);
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_BIND_READY:
			if (!wait_for_msg_from(fefd, &msg, 1)) {
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_BIND:
				relay_msg_to(befd, &msg);
				state = FSM_BIND_SENT;
				break;

			default:
				send_error_to(fefd, ERR_BADPROTO);
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_BIND_SENT:
			if (!wait_for_msg_from(befd, &msg, 1)) {
				send_error_to(fefd, ERR_BEFAIL);
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_ERROR_RESPONSE:
				relay_msg_to(fefd, &msg);
				state = FSM_SHUTDOWN;
				break;

			case PG3_MSG_BIND_COMPLETE:
				relay_msg_to(fefd, &msg);
				state = FSM_EXEC_READY;
				break;

			default:
				send_error_to(fefd, ERR_BADPROTO);
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_EXEC_READY:
			if (!wait_for_msg_from(fefd, &msg, 1)) {
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_EXECUTE:
				relay_msg_to(befd, &msg);
				state = FSM_EXEC_SENT;
				break;

			default:
				send_error_to(fefd, ERR_BADPROTO);
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_EXEC_SENT:
			if (!wait_for_msg_from(befd, &msg, 1)) {
				send_error_to(fefd, ERR_BEFAIL);
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_ERROR_RESPONSE:
				relay_msg_to(fefd, &msg);
				state = FSM_SHUTDOWN;
				break;

			case PG3_MSG_COMMAND_COMPLETE:
				relay_msg_to(fefd, &msg);
				state = FSM_WAIT_SYNC;
				break;

			case PG3_MSG_COPY_IN_RESPONSE:
			case PG3_MSG_COPY_OUT_RESPONSE:
				/* FIXME: handle copy in / copy out */
				pgr_abort(ABORT_UNIMPL);

			case PG3_MSG_DATA_ROW:
			case PG3_MSG_EMPTY_QUERY_RESPONSE:
				relay_msg_to(fefd, &msg);
				break;

			default:
				send_error_to(fefd, ERR_BADPROTO);
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_WAIT_SYNC:
			if (!wait_for_msg_from(fefd, &msg, 1)) {
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_SYNC:
				relay_msg_to(befd, &msg);
				state = FSM_SYNC_SENT;
				break;

			default:
				send_error_to(fefd, ERR_BADPROTO);
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		case FSM_SYNC_SENT:
			if (!wait_for_msg_from(befd, &msg, 1)) {
				send_error_to(fefd, ERR_BEFAIL);
				state = FSM_SHUTDOWN;
				break;
			}

			switch (msg.type) {
			case PG3_MSG_READY_FOR_QUERY:
				relay_msg_to(befd, &msg);
				state = FSM_READY;
				break;

			default:
				send_error_to(fefd, ERR_BADPROTO);
				state = FSM_SHUTDOWN;
				break;
			}
			break;

		default:
			pgr_debugf("UNKNOWN FSM STATE %d (%#02x)", state, state);
			pgr_logf(stderr, LOG_ERR, "finite state machine appears to be in a bad state (%d / %#02x); please file a bug report",
					state, state);
			pgr_abort(ABORT_ABSURD);
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
