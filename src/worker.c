#include "pgrouter.h"
#include "proto.h"
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>

#define SUBSYS "worker"
#include "locks.inc.c"

#define max(a,b) ((a) > (b) ? (a) : (b))

static void handle_client(CONTEXT *c, int connfd)
{
	int rc, backend;
	PG3_MSG msg;
	PG3_ERROR err;

	backend = pgr_pick_any(c);

	/* FIXME: wait for startup message from client; relay to chosen slave */
	rc = pg3_recv(connfd, &msg, PG3_MSG_STARTUP);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "failed to receive startup message from client: %s (errno %d, rc %d)",
				strerror(errno), errno, rc);
		return;
	}

	if (backend < 0) {
		pgr_logf(stderr, LOG_ERR, "unable to find a suitable backend for initial stage of conversation");
		err.severity = "ERROR";
		err.sqlstate = "08000"; /* connection exception */
		err.message  = "No viable backends found";
		err.details  = "pgrouter was unable to find a healthy PostgreSQL backend";
		err.hint     = "Check the health of your PostgreSQL backend clusters, and make sure that they are up, accepting connections, and within the replication lag threshold from the master";

		pg3_free(&msg);
		rc = pg3_error(&msg, &err);
		if (rc != 0) {
			pgr_logf(stderr, LOG_ERR, "failed to allocate a ErrorResponse '%s': %s (errno %d)",
					err.message, strerror(errno), errno);
			return;
		}

		rc = pg3_send(connfd, &msg);
		if (rc != 0) {
			pgr_logf(stderr, LOG_ERR, "failed to send ErrorResponse '%s': %s (errno %d)",
					err.message, strerror(errno), errno);
			return;
		}

		pgr_logf(stderr, LOG_INFO, "ErrorResponse '%s' sent; disconnecting", err.message);
		pg3_free(&msg);
		return;
	}

	/* FIXME: connect to the backend */;
	/* FIXME: relay the Startup message to our backend */
	/* FIXME: if backend replies with an auth response */
		/* FIXME: check that it is md5 - if not, forge error to client and close */
		/* FIXME: relay md5 packet to slave; relay response to client */
		/* FIXME: save md6 packet for later (if we need to auth to a master) */
	/* FIXME: process any startup messages from slave, until ReadyForQuery */
	/* FIXME: enter main protocol loop */
		/* FIXME: listen for a query from the client: */
			/* FIXME: if it is an updating query, connect to a master (+auth) */
			/* FIXME: otherwise, use last connected (master / slave) */
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
				/* FIXME: acquire wrlock on CONTEXT to update #clients (++) */
				handle_client(c, connfd);
				/* FIXME: acquire wrlock on CONTEXT to update #clients (--) */
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
