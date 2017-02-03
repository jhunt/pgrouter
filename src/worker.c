/*
  Copyright (c) 2016 James Hunt

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
 */

#include "pgrouter.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>

#define SUBSYS "worker"
#include "locks.inc.c"

#define max(a,b) ((a) > (b) ? (a) : (b))

static double time_ms()
{
	int rc;
	struct timeval tv;

	rc = gettimeofday(&tv, NULL);
	if (rc) {
		pgr_debugf("gettimeofday() failed: %s (errno %d)", strerror(errno), errno);
		pgr_abort(ABORT_ABSURD);
	}

	return tv.tv_sec + (tv.tv_usec / 1000000.0);
}

static void dump_timer(const char *type, double start, double end)
{
	pgr_debugf("[TIMER] %s: %0.3lf elapsed", type, end - start);
}

static struct {
	double start, end;
	int x;
} WATCH;

#define TIMER(m) for (WATCH.start = time_ms(), WATCH.x = 0; WATCH.x != 1; WATCH.end = time_ms(), WATCH.x = 1, dump_timer((m), WATCH.start, WATCH.end))


static int determine_backends(CONTEXT *c, CONNECTION *reader, CONNECTION *writer)
{
	int rc, i;
	int *weights;
	int cumulative;

	rdlock(&c->lock, "context", 0);

	cumulative = 0;
	weights = calloc(c->num_backends, sizeof(int));
	if (!weights) {
		pgr_abort(ABORT_MEMFAIL);
	}

	for (i = 0; i < c->num_backends; i++) {
		rdlock(&c->backends[i].lock, "backend", i);

		if (c->backends[i].role == BACKEND_ROLE_MASTER) {
			writer->serial   = c->backends[i].serial;
			writer->index    = i;
			writer->hostname = strdup(c->backends[i].hostname);
			writer->port     = c->backends[i].port;
			writer->timeout  = c->health.timeout * 1000;

		} else if (c->backends[i].status == BACKEND_IS_OK &&
		           c->backends[i].health.lag < c->backends[i].health.threshold) {
			cumulative += c->backends[i].weight;
			weights[i] = cumulative;
		}

		unlock(&c->backends[i].lock, "backend", i);
	}

	if (cumulative == 0) {
		pgr_logf(stderr, LOG_ERR, "[worker] no backends are viable!!");
		unlock(&c->lock, "context", 0);
		free(weights);
		return -1;
	}

	int r = pgr_rand(0, cumulative);
	pgr_debugf("picking backend using random value %d from (%d,%d)", r, 0, cumulative);
	for (i = 0; i < c->num_backends; i++) {
		pgr_debugf("checking backend %d (cumulative weight %d) against %d", i, weights[i], r);
		if (r <= weights[i]) {
			rdlock(&c->backends[i].lock, "backend", i);

			reader->serial   = c->backends[i].serial;
			reader->index    = i;
			reader->hostname = strdup(c->backends[i].hostname);
			reader->port     = c->backends[i].port;
			reader->timeout  = c->health.timeout * 1000;

			pgr_logf(stderr, LOG_INFO, "[worker] using backend %d, %s:%d (serial %d)",
					reader->index, reader->hostname, reader->port, reader->serial);

			unlock(&c->backends[i].lock, "backend", i);
			unlock(&c->lock, "context", 0);
			free(weights);
			return 0;
		}
	}

	unlock(&c->lock, "context", 0);
	free(weights);
	return -1;
}

static int query(MBUF *m, const char *query)
{
	size_t len = htonl(4 + strlen(query) + 1);
	pgr_mbuf_cat(m, "Q", 1);
	pgr_mbuf_cat(m, &len, 4);
	pgr_mbuf_cat(m, query, strlen(query) + 1);
	return 0;
}

static int xlog(const char *s, lag_t *lag)
{
	const char *p;
	lag_t hi = 0, lo = 0;

	pgr_debugf("parsing xlog value '%s'", s);
	for (p = s; *p && *p != '/'; p++) {
		if (*p >= '0' || *p <= '9') {
			hi = hi * 0xf + (*p - '0');
		} else if (*p >= 'a' || *p <= 'f') {
			hi = hi * 0xf + (*p - 'a');
		} else if (*p >= 'A' || *p <= 'F') {
			hi = hi * 0xf + (*p - 'A');
		} else {
			pgr_logf(stderr, LOG_ERR, "[worker] invalid character '%c' found in xlog value %s", *p, s);
			return 1;
		}
	}

	if (*p != '/') {
		pgr_logf(stderr, LOG_ERR, "[worker] malformed xlog value %s", s);
		return 1;
	}

	for (p++; *p; p++) {
		if (*p >= '0' || *p <= '9') {
			lo = lo * 0xf + (*p - '0');
		} else if (*p >= 'a' || *p <= 'f') {
			lo = lo * 0xf + (*p - 'a');
		} else if (*p >= 'A' || *p <= 'F') {
			lo = lo * 0xf + (*p - 'A');
		} else {
			pgr_logf(stderr, LOG_ERR, "[worker] invalid character '%c' found in xlog value %s", *p, s);
			return 1;
		}
	}

	*lag = (hi << 32) | lo;
	return 0;
}

static lag_t get_xlog_location(MBUF *m, const char *sql)
{
	int rc, len;
	lag_t offset = 0;
	char *buf;

	/* SEND the query */
	query(m, sql);
	rc = pgr_mbuf_relay(m);
	if (rc != 0) {
		return (lag_t)0;
	}

	/* RECV the RowDescription */
	rc = pgr_mbuf_recv(m);
	if (rc < 0) {
		return (lag_t)0;
	}
	if (pgr_mbuf_msgtype(m) != 'T') {
		goto drain;
	}
	if (pgr_mbuf_u16(m, 0) != 1) {
		pgr_logf(stderr, LOG_ERR, "Invalid number of columns (%d, not 1) in response to `%s`",
				pgr_mbuf_u16(m, 0), sql);
		goto drain;
	}
	pgr_mbuf_discard(m);

	/* RECV the DataRow */
	rc = pgr_mbuf_recv(m);
	if (rc < 0) {
		return (lag_t)0;
	}
	if (pgr_mbuf_msgtype(m) != 'D') {
		goto drain;
	}
	/* extract the text value from column 0 */
	len = pgr_mbuf_u32(m, 2);
	if (len == 0 || len > 0xff) {
		pgr_logf(stderr, LOG_ERR, "suspicious looking xlog value length (%d)\n", len);
		goto drain;
	}
	buf = calloc(len + 1, 1);
	if (!buf) {
		pgr_abort(ABORT_MEMFAIL);
	}
	memcpy(buf, pgr_mbuf_data(m, 6, len), len);
	rc = xlog(buf, &offset);
	if (rc != 0) {
		offset = 0;
	}
	pgr_mbuf_discard(m);

	/* RECV the CommandComplete */
	rc = pgr_mbuf_recv(m);
	if (rc < 0) {
		return (lag_t)0;
	}
	if (pgr_mbuf_msgtype(m) != 'C') {
		goto drain;
	}
	pgr_mbuf_discard(m);

	return offset;

drain:
	pgr_mbuf_drain(m, 'Z');
	return (lag_t)0;
}

static lag_t get_master_offset(MBUF *m)
{
	return get_xlog_location(m, "SELECT pg_current_xlog_location()");
}

static lag_t get_slave_offset(MBUF *m)
{
	return get_xlog_location(m, "SELECT pg_last_xlog_receive_location()");
}

static void handle_client(CONTEXT *c, int fd)
{
	CONNECTION frontend, reader, writer;
	int rc, befd, in_txn, len;
	char type;
	MBUF *fe, *be;
	lag_t wr_xlog = 0;

	fe = pgr_mbuf_new(16384);
	be = pgr_mbuf_new(4096);

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

	pgr_mbuf_setfd(fe, fd, MBUF_NO_FD);
	pgr_mbuf_setfd(be, MBUF_NO_FD, fd);
	befd = reader.fd;

	in_txn = 0;
	for (;;) {
		if (befd == writer.fd && !in_txn) {
			if (wr_xlog > 1) {
				pgr_debugf("checking to see if our reader has caught up with our writer");
				/* FIXME: may want to defer to the watcher, instead */
				pgr_mbuf_setfd(be, reader.fd, reader.fd);
				if (get_slave_offset(be) >= wr_xlog) {
					wr_xlog = 0;
					befd = reader.fd;
				}
			} else {
				befd = reader.fd;
			}
		}

		pgr_mbuf_setfd(fe, fd, befd);
		pgr_mbuf_setfd(be, befd, fd);

		do {
			pgr_debugf("reading message from frontend");
			rc = pgr_mbuf_recv(fe);
			if (rc < 0) {
				goto shutdown;
			}

			type = pgr_mbuf_msgtype(fe);
			len  = pgr_mbuf_msglength(fe);

			if (type == 'Q') {
				char *query;
				query = pgr_mbuf_data(fe, 0, 5);
				if (query && strncasecmp(query, "begin", 5) == 0) {
					in_txn = 1;
					befd = writer.fd; /* force transactions to writer */
					pgr_mbuf_setfd(fe, fd, writer.fd);
				}

				query = pgr_mbuf_data(fe, 0, 6);
				if (query && strncasecmp(query, "commit", 6) == 0) {
					in_txn = 0;
				}
			}

			pgr_debugf("sending message to %s (fd %d)",
					befd == reader.fd ? "reader" : "writer", befd);
			rc = pgr_mbuf_send(fe);
			if (rc != 0) {
				goto shutdown;
			}

			if (type == 'X') {
				pgr_sendn(reader.fd, "X\0\0\0\x4", 5);
				pgr_sendn(writer.fd, "X\0\0\0\x4", 5);
				goto shutdown;
			}
		} while (type != 'Q' && type != 'S');

		do {
again:
			pgr_debugf("reading message from %s (fd %d)",
					befd == reader.fd ? "reader" : "writer", befd);
			rc = pgr_mbuf_recv(be);
			if (rc < 0) {
				goto shutdown;
			}

			type = pgr_mbuf_msgtype(be);

			if (pgr_mbuf_iserror(be, "25006") == 0 && befd == reader.fd) {
				pgr_debugf("E25006 bad routing - ignoring remaining backend messages...");
				pgr_mbuf_drain(be, 'Z');

				befd = writer.fd;
				pgr_mbuf_setfd(fe, fd, befd);
				pgr_mbuf_setfd(be, befd, fd);
				pgr_debugf("resending saved messages to writer (fd %d)", befd);
				pgr_mbuf_resend(fe);
				pgr_mbuf_reset(fe);
				goto again;
			}

			/* handle CopyInResponse by switching to sub-protocol */
			if (type == 'G') {
				pgr_debugf("relaying message to frontend (fd %d)", frontend.fd);
				rc = pgr_mbuf_relay(be);
				if (rc != 0) {
					goto shutdown;
				}

				pgr_debugf("switching to COPY DATA sub-protocol");
				do {
					pgr_debugf("reading message from frontend (fd %d)", frontend.fd);
					rc = pgr_mbuf_recv(fe);
					if (rc < 0) {
						goto shutdown;
					}

					type = pgr_mbuf_msgtype(fe);

					pgr_debugf("relaying message to %s (fd %d)",
							befd == reader.fd ? "reader" : "writer", befd);
					rc = pgr_mbuf_relay(fe);
					if (rc != 0) {
						goto shutdown;
					}
				} while (type != 'c' && type != 'F');
				pgr_debugf("returning to NORMAL protocol");
				continue;
			}

			pgr_debugf("relaying message to frontend (fd %d)", frontend.fd);
			rc = pgr_mbuf_relay(be);
			if (rc != 0) {
				goto shutdown;
			}

		} while (type != 'Z');

		/* FIXME: only do this on success? */
		wr_xlog = 0;
		if (befd == writer.fd) {
			pgr_mbuf_setfd(be, befd, befd);
			pgr_debugf("we sent a query to the writer, querying it for its new xlog offset");
			wr_xlog = get_master_offset(be);
		}
	}
shutdown:
	pgr_debugf("closing all frontend and backend connections");
	pgr_conn_deinit(&reader); free(reader.hostname);
	pgr_conn_deinit(&writer); free(writer.hostname);
	pgr_conn_deinit(&frontend);
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
				double t;
				char remote_addr[INET6_ADDRSTRLEN+1];
				struct sockaddr_storage peer;
				int peer_len = sizeof(peer);

				connfd = accept(watch[i], (struct sockaddr*)&peer, &peer_len);
				wrlock(&c->lock, "context", 0);
				c->fe_conns++;
				unlock(&c->lock, "context", 0);

				switch (peer.ss_family) {
				case AF_INET:
					memset(remote_addr, 0, sizeof(remote_addr));
					inet_ntop(AF_INET, &((struct sockaddr_in*)&peer)->sin_addr,
						remote_addr, sizeof(remote_addr)),
					pgr_logf(stderr, LOG_INFO, "[worker] inbound connection from %s:%d",
							remote_addr, ((struct sockaddr_in*)&peer)->sin_port);
					break;

				case AF_INET6:
					memset(remote_addr, 0, sizeof(remote_addr));
					inet_ntop(AF_INET6, &((struct sockaddr_in6*)&peer)->sin6_addr,
						remote_addr, sizeof(remote_addr)),
					pgr_logf(stderr, LOG_INFO, "[worker] inbound connection from %s:%d",
							remote_addr, ((struct sockaddr_in6*)&peer)->sin6_port);
					break;
				}

				pgr_msgf(stderr, "Handling new inbound client connection (fd %d)", connfd);
				t = time_ms();
				handle_client(c, connfd);
				t = time_ms() - t;
				pgr_logf(stderr, LOG_INFO, "Client connection (fd %d) completed in %lfs",
						connfd, t);

				wrlock(&c->lock, "context", 0);
				c->fe_conns--;
				unlock(&c->lock, "context", 0);

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
