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

static int writef(int fd, const char *fmt, ...)
{
	int n, wr;
	char *buf;
	va_list ap;

	va_start(ap, fmt);
	n = vasprintf(&buf, fmt, ap);
	va_end(ap);

	if (n > 0 && buf[n - 1] == '\n') {
		buf[n-1] = '.';
		pgr_logf(stderr, LOG_DEBUG, "[monitor] writing %d/%d bytes to fd %d [%s]", n, n, fd, buf);
		buf[n-1] = '\n';
	} else {
		pgr_logf(stderr, LOG_DEBUG, "[monitor] writing %d/%d bytes to fd %d [%s]", n, n, fd, buf);
	}
	wr = 0;
	while ((wr = write(fd, buf + wr, n)) > 0) {
		pgr_logf(stderr, LOG_DEBUG, "[monitor]   wrote %d/%d bytes to fd %d (%d remain)",
			wr, n, fd, n - wr);
		n -= wr;
	}
	if (wr < 0) {
		return wr;
	}
	return n;
}

static void handle_client(CONTEXT *c, int connfd)
{
	int rc, i;

	rc = rdlock(&c->lock, "context", 0);
	if (rc != 0) {
		return;
	}

	writef(connfd, "backends %d/%d\n", c->ok_backends, c->num_backends);
	writef(connfd, "workers %d\n", c->workers);
	writef(connfd, "clients ??\n"); /* FIXME: get real data */
	writef(connfd, "connections ??\n"); /* FIXME: get real data */

	for (i = 0; i < c->num_backends; i++) {
		rc = rdlock(&c->backends[i].lock, "backend", i);
		if (rc != 0) {
			return;
		}

		if (c->backends[i].status == BACKEND_IS_OK) {
			writef(connfd, "%s:%d %s OK %llu/%llu\n",
					c->backends[i].hostname, c->backends[i].port,
					(c->backends[i].master ? "master" : "slave"),
					c->backends[i].health.lag,
					c->backends[i].health.threshold);
		} else {
			writef(connfd, "%s:%d %s %s\n",
					c->backends[i].hostname, c->backends[i].port,
					(c->backends[i].master ? "master" : "slave"),
					pgr_backend_status(c->backends[i].status));
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
			pgr_logf(stderr, LOG_ERR, "select recived system error: %s (errno %d)",
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
