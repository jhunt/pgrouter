#include "pgrouter.h"
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>

#define SUBSYS "monitor"
#include "locks.inc.c"

#define max(a,b) ((a) > (b) ? (a) : (b))

static void handle_client(CONTEXT *c, int connfd)
{
	int rc, i;

	rc = rdlock(&c->lock, "context", 0);
	if (rc != 0) {
		return;
	}

	pgr_sendf(connfd, "backends %d/%d\n", c->ok_backends, c->num_backends);
	pgr_sendf(connfd, "workers %d\n", c->workers);
	pgr_sendf(connfd, "clients ??\n"); /* FIXME: get real data */
	pgr_sendf(connfd, "connections ??\n"); /* FIXME: get real data */

	for (i = 0; i < c->num_backends; i++) {
		rc = rdlock(&c->backends[i].lock, "backend", i);
		if (rc != 0) {
			return;
		}

		switch (c->backends[i].status) {
		case BACKEND_IS_OK:
			pgr_sendf(connfd, "%s:%d %s %s %llu/%llu\n",
					c->backends[i].hostname, c->backends[i].port,
					pgr_backend_role(c->backends[i].role),
					pgr_backend_status(c->backends[i].status),
					c->backends[i].health.lag,
					c->backends[i].health.threshold);
			break;

		default:
			pgr_sendf(connfd, "%s:%d %s %s\n",
					c->backends[i].hostname, c->backends[i].port,
					pgr_backend_role(c->backends[i].role),
					pgr_backend_status(c->backends[i].status));
			break;
		}

		rc = unlock(&c->backends[i].lock, "backend", i);
		if (rc != 0) {
			return;
		}
	}

	rc = unlock(&c->lock, "context", 0);
	if (rc != 0) {
		return;
	}
}

static void* do_monitor(void *_c)
{
	CONTEXT *c = (CONTEXT*)_c;
	int rc, connfd, i, nfds;
	int watch[2] = { c->monitor4, c->monitor6 };
	fd_set rfds;

	/* FIXME: we should pass a new sockaddr to accept() and log about remote clients */

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
			pgr_logf(stderr, LOG_ERR, "[monitor] select received system error: %s (errno %d)",
					strerror(errno), errno);
			pgr_abort(ABORT_SYSCALL);
		}

		for (i = 0; i < sizeof(watch)/sizeof(watch[0]); i++) {
			if (watch[i] >= 0 && FD_ISSET(watch[i], &rfds)) {
				connfd = accept(watch[i], NULL, NULL);
				handle_client(c, connfd);
				close(connfd);
			}
		}
	}

	close(c->monitor4);
	close(c->monitor6);
	return NULL;
}

int pgr_monitor(CONTEXT *c, pthread_t *tid)
{
	int rc = pthread_create(tid, NULL, do_monitor, c);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[monitor] failed to spin up: %s (errno %d)",
				strerror(errno), errno);
		return;
	}

	pgr_logf(stderr, LOG_INFO, "[monitor] spinning up [tid=%i]", *tid);
}
