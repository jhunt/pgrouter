#include "pgrouter.h"
#include "proto.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#define SUBSYS "worker"
#include "locks.inc.c"

#define max(a,b) ((a) > (b) ? (a) : (b))

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

static int determine_backends(CONTEXT *c, CONNECTION *reader, CONNECTION *writer)
{
	int rc, i;
	int *weights;
	int cumulative;

	rc = rdlock(&c->lock, "context", 0);
	if (rc != 0) {
		return rc;
	}

	cumulative = 0;
	weights = calloc(c->num_backends, sizeof(int));
	if (!weights) {
		pgr_abort(ABORT_MEMFAIL);
	}

	for (i = 0; i < c->num_backends; i++) {
		rc = rdlock(&c->backends[i].lock, "backend", i);
		if (rc != 0) {
			unlock(&c->lock, "context", 0);
			return rc;
		}

		if (c->backends[i].role == BACKEND_ROLE_MASTER) {
			writer->serial   = c->backends[i].serial;
			writer->index    = i;
			writer->hostname = strdup(c->backends[i].hostname);
			writer->port     = c->backends[i].port;

		} else if (c->backends[i].status == BACKEND_IS_OK &&
		           c->backends[i].health.lag < c->backends[i].health.threshold) {
			cumulative += c->backends[i].weight;
			weights[i] = cumulative;
		}

		rc = unlock(&c->backends[i].lock, "backend", i);
		if (rc != 0) {
			unlock(&c->lock, "context", 0);
			return rc;
		}
	}

	if (cumulative == 0) {
		pgr_logf(stderr, LOG_ERR, "[worker] no backends are viable!!");
		unlock(&c->lock, "context", 0);
		return -1;
	}

	int r = pgr_rand(0, cumulative);
	pgr_debugf("picking backend using random value %d from (%d,%d)", r, 0, cumulative);
	for (i = 0; i < c->num_backends; i++) {
		pgr_debugf("checking backend %d (cumulative weight %d) against %d", i, weights[i], r);
		if (r <= weights[i]) {
			rc = rdlock(&c->backends[i].lock, "backend", i);
			if (rc != 0) {
				unlock(&c->lock, "context", 0);
				return -1;
			}

			reader->serial   = c->backends[i].serial;
			reader->index    = i;
			reader->hostname = strdup(c->backends[i].hostname);
			reader->port     = c->backends[i].port;
			r = -1; /* found it */

			pgr_logf(stderr, LOG_INFO, "[worker] using backend %d, %s:%d (serial %d)",
					reader->index, reader->hostname, reader->port, reader->serial);

			rc = rdlock(&c->backends[i].lock, "backend", i);
			if (rc != 0) {
				unlock(&c->lock, "context", 0);
				return -1;
			}

			rc = unlock(&c->lock, "context", 0);
			return rc;
		}
	}

	rc = unlock(&c->lock, "context", 0);
	if (rc != 0) {
		return rc;
	}

	return r == -1 ? -1 : 0;
}

static void handle_client(CONTEXT *c, int fd)
{
	int rc, i, state;
	CONNECTION frontend, reader, writer;
	PG3_MSG msg;

	pgr_conn_init(c, &frontend);
	pgr_conn_init(c, &reader);
	pgr_conn_init(c, &writer);

	pgr_conn_frontend(&frontend, fd);

	if (pgr_conn_accept(&frontend)              != 0 ||
	    determine_backends(c, &reader, &writer) != 0 ||
	    pgr_conn_copy(&reader, &frontend)       != 0 ||
	    pgr_conn_copy(&writer, &frontend)       != 0 ||
	    pgr_conn_connect(&reader)               != 0 ||
	    pgr_conn_connect(&writer)               != 0) {
		goto shutdown;
	}

	state = FSM_READY;
	for (;;) {
		pgr_debugf("FSM[%s]", fsm_name(state));
		switch (state) {
		case FSM_READY:
			do {
				pgr_debugf("receiving message from frontend (fd %d)", frontend.fd);
				rc = pg3_recv(frontend.fd, &msg, 1);
				if (rc != 0) {
					goto shutdown;
				}

				pgr_debugf("relaying message from frontend (fd %d) to reader (fd %d)",
						frontend.fd, reader.fd);
				rc = pg3_send(reader.fd, &msg);
				if (rc != 0) {
					goto shutdown;
				}
			} while (msg.type != 'Q' && msg.type != 'S');

			do {
				pgr_debugf("receiving message from reader (fd %d)", reader.fd);
				rc = pg3_recv(reader.fd, &msg, 1);
				if (rc != 0) {
					goto shutdown;
				}

				pgr_debugf("relaying message from reader (fd %d) to frontend (fd %d)",
						reader.fd, frontend.fd);
				rc = pg3_send(frontend.fd, &msg);
				if (rc != 0) {
					goto shutdown;
				}
			} while (msg.type != 'Z');
			break;

		case FSM_QUERY_SENT:
		case FSM_PARSE_SENT:
		case FSM_BIND_READY:
		case FSM_BIND_SENT:
		case FSM_EXEC_READY:
		case FSM_EXEC_SENT:
		case FSM_WAIT_SYNC:
		case FSM_SYNC_SENT:
			break;
		}

	}
shutdown:
	pgr_debugf("closing all frontend and backend connection");
	close(reader.fd);
	close(writer.fd);
	close(frontend.fd);
	return;
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
