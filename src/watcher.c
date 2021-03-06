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
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <libpq-fe.h>

#define SUBSYS "watcher"
#include "locks.inc.c"

typedef struct {
	int serial;         /* BACKEND.serial; used to detect changes      */
	int timeout;        /* health check connection timeout, in seconds */

	int ok;             /* is the backend is accepting connections?    */
	int role;           /* is the backend the write master or a slave? */
	lag_t pos;          /* xlog position for this backend (absolute)   */

	char endpoint[256]; /* "<host>:<port>" string, for diagnostics     */
	char userdb[256];   /* "<user>@<database>" string, for diagnostics */
	char dsn[1024];     /* full PostgreSQL DSN for the health database */
} HEALTH;

static int NUM_BACKENDS; /* how many backends are there?               */
static HEALTH *BACKENDS; /* health information, cached for speed       */

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
			pgr_logf(stderr, LOG_ERR, "[watcher] invalid character '%c' found in xlog value %s", *p, s);
			return 1;
		}
	}

	if (*p != '/') {
		pgr_logf(stderr, LOG_ERR, "[watcher] malformed xlog value %s", s);
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
			pgr_logf(stderr, LOG_ERR, "[watcher] invalid character '%c' found in xlog value %s", *p, s);
			return 1;
		}
	}

	*lag = (hi << 32) | lo;
	return 0;
}

static void* do_watcher(void *_c)
{
	int sleep_for, n;
	CONTEXT *c = (CONTEXT*)_c;
	int rc;

	for (;;) {
		rdlock(&c->lock, "context", 0);

		/* if we added or removed backends as part of a configuration
		   reload, we need to reallocate the cached health information. */
		if (c->num_backends != NUM_BACKENDS) {
			pgr_debugf("number of backends changed (old %d != new %d); "
					"reallocating internal structures that keep track of backend health...",
					NUM_BACKENDS, c->num_backends);

			free(BACKENDS);
			NUM_BACKENDS = c->num_backends;
			BACKENDS = calloc(NUM_BACKENDS, sizeof(HEALTH));
			if (!BACKENDS) {
				pgr_logf(stderr, LOG_ERR, "[watcher failed to allocate memory during initialization: %s (errno %d)",
						strerror(errno), errno);
				unlock(&c->lock, "context", 0);
				pgr_abort(ABORT_MEMFAIL);
			}
		}

		/* iterate over the backends and extract whatever information
		   we need to while we have their respective read locks. */
		int i;
		for (i = 0; i < NUM_BACKENDS; i++) {
			rdlock(&c->backends[i].lock, "backend", i);

			if (c->backends[i].serial != BACKENDS[i].serial) {
				pgr_debugf("backend/%d cached serial %d != actual serial %d; updating cache entry",
						i, BACKENDS[i].serial, c->backends[i].serial);
				pgr_logf(stderr, LOG_INFO, "[watcher] updating backend/%d with (potential) new connection information", i);

				BACKENDS[i].timeout = c->health.timeout;
				pgr_debugf("backend/%d: setting health check timeout to %ds",
						i, BACKENDS[i].timeout);

				/* endpoint = "host:port" */
				n = snprintf(BACKENDS[i].endpoint, sizeof(BACKENDS[i].endpoint),
						"%s:%d", c->backends[i].hostname, c->backends[i].port);
				pgr_debugf("backend/%d: setting endpoint to '%s'",
						i, BACKENDS[i].endpoint);
				if (n >= sizeof(BACKENDS[i].endpoint)) {
					pgr_logf(stderr, LOG_ERR, "[watcher] re-cache of backend/%d endpoint string failed; "
							"'%s:%d' is more than %d bytes long",
							i, c->backends[i].hostname, c->backends[i].port,
							sizeof(BACKENDS[i].endpoint) - 1);
				}

				/* userdb = "user@database" */
				n = snprintf(BACKENDS[i].userdb, sizeof(BACKENDS[i].userdb),
						"%s@%s", c->backends[i].health.username, c->backends[i].health.database);
				pgr_debugf("backend/%d: setting user@db to '%s'",
						i, BACKENDS[i].userdb);
				if (n >= sizeof(BACKENDS[i].userdb)) {
					pgr_logf(stderr, LOG_ERR, "[watcher] re-cache of backend/%d user@db string failed; "
							"'%s@%s' is more than %d bytes long",
							i, c->backends[i].health.username, c->backends[i].health.database,
							sizeof(BACKENDS[i].userdb) - 1);
				}

				n = snprintf(BACKENDS[i].dsn, sizeof(BACKENDS[i].dsn),
						"host=%s port=%d user=%s password='%s' dbname=%s "
						"connect_timeout=%d application_name=pgrouter",
						c->backends[i].hostname, c->backends[i].port,
						c->backends[i].health.username, c->backends[i].health.password,
						c->backends[i].health.database, BACKENDS[i].timeout);
				pgr_debugf("backend/%d: setting dsn to '%s'",
						i, BACKENDS[i].dsn);
				if (n >= sizeof(BACKENDS[i].dsn)) {
					pgr_logf(stderr, LOG_ERR, "[watcher] re-cache of backend/%d dsn string failed; "
							"'host=%s port=%d user=%s password'%s' dbname=%s "
							"connect_timeout=%d application=pgrouter' is more than %d bytes long",
							i, c->backends[i].hostname, c->backends[i].port,
							c->backends[i].health.username, c->backends[i].health.password,
							c->backends[i].health.database, BACKENDS[i].timeout,
							sizeof(BACKENDS[i].dsn) - 1);
				}

				BACKENDS[i].serial = c->backends[i].serial;
				pgr_debugf("backend/%d: setting serial to %d",
						i, BACKENDS[i].serial);

				BACKENDS[i].role = c->backends[i].role;
			}

			unlock(&c->backends[i].lock, "backend", i);
		}
		unlock(&c->lock, "context", 0);

		/* now, loop over the backends and gather our health data */
		int master_pos;
		for (i = 0; i < NUM_BACKENDS; i++) {
			BACKENDS[i].ok   = BACKEND_IS_FAILED;
			BACKENDS[i].pos  = 0;

			pgr_logf(stderr, LOG_INFO, "[watcher] checking backend/%d %s (connecting as %s)",
					i, BACKENDS[i].endpoint, BACKENDS[i].userdb);
			pgr_debugf("connecting with dsn '%s'", BACKENDS[i].dsn);

			PGconn *conn = PQconnectdb(BACKENDS[i].dsn);
			if (!conn) {
				pgr_logf(stderr, LOG_ERR, "[watcher] failed to allocate memory for connection to %s backend",
					BACKENDS[i].endpoint);
				pgr_abort(ABORT_MEMFAIL);
			}

			switch (rc = PQstatus(conn)) {
			case CONNECTION_BAD:
				pgr_logf(stderr, LOG_ERR, "[watcher] failed to connect to %s backend: %s",
					BACKENDS[i].endpoint, PQerrorMessage(conn));
				break;

			case CONNECTION_OK:
				pgr_logf(stderr, LOG_INFO, "[watcher] connected to %s backend",
					BACKENDS[i].endpoint);

				/* determine if master or slave `SELECT pg_is_in_recovery()` */
				PGresult *result;
				const char *sql = "SELECT pg_is_in_recovery()";
				result = PQexec(conn, sql);
				if (!result) {
					pgr_logf(stderr, LOG_ERR, "[watcher] failed to allocate memory for result of `%s` query", sql);
					pgr_abort(ABORT_MEMFAIL);
				}
				if (PQresultStatus(result) != PGRES_TUPLES_OK) {
					pgr_logf(stderr, LOG_ERR, "[watcher] got an unexpected %s from %s backend, in response to `%s` query",
							PQresStatus(PQresultStatus(result)), BACKENDS[i].endpoint, sql);
					BACKENDS[i].ok = BACKEND_IS_HALFUP;
					PQclear(result);
					break;
				}

				/* we should get exactly 1 result from `SELECT pg_is_in_recovery()` */
				if (PQntuples(result) != 1) {
					pgr_logf(stderr, LOG_ERR, "[watcher] received %d results from `%s` query on %s backend (expected just one)",
							PQntuples(result), sql, BACKENDS[i].endpoint);
					BACKENDS[i].ok = BACKEND_IS_HALFUP;
					PQclear(result);
					break;
				}

				/* that result should have exactly one field */
				if (PQnfields(result) != 1) {
					pgr_logf(stderr, LOG_ERR, "[watcher] received %d columns in result from `%s` query on %s backend (expected just one)",
							PQnfields(result), sql, BACKENDS[i].endpoint);
					BACKENDS[i].ok = BACKEND_IS_HALFUP;
					PQclear(result);
					break;
				}

				char *val = PQgetvalue(result, 0, 0);
				pgr_logf(stderr, LOG_INFO, "backend %s returned '%s' for `%s`",
						BACKENDS[i].endpoint, val, sql);
				BACKENDS[i].role = *val == 't' ? BACKEND_ROLE_SLAVE : BACKEND_ROLE_MASTER;
				PQclear(result);

				/* determine xlog position */
				if (BACKENDS[i].role == BACKEND_ROLE_MASTER) {
					sql = "SELECT pg_current_xlog_location()";
				} else {
					sql = "SELECT pg_last_xlog_receive_location()";
				}
				result = PQexec(conn, sql);
				if (!result) {
					pgr_logf(stderr, LOG_ERR, "[watcher] failed to allocate memory for result of `%s` query", sql);
					pgr_abort(ABORT_MEMFAIL);
				}
				if (PQresultStatus(result) != PGRES_TUPLES_OK) {
					pgr_logf(stderr, LOG_ERR, "[watcher] got an unexpected %s from %s backend, in response to `%s` query",
							PQresStatus(PQresultStatus(result)), BACKENDS[i].endpoint, sql);
					BACKENDS[i].ok = BACKEND_IS_HALFUP;
					PQclear(result);
					break;
				}

				/* we should get exactly 1 result */
				if (PQntuples(result) != 1) {
					pgr_logf(stderr, LOG_ERR, "[watcher] received %d results from `%s` query on %s backend (expected just one)",
							PQntuples(result), sql, BACKENDS[i].endpoint);
					BACKENDS[i].ok = BACKEND_IS_HALFUP;
					PQclear(result);
					break;
				}

				/* that result should have exactly one field */
				if (PQnfields(result) != 1) {
					pgr_logf(stderr, LOG_ERR, "[watcher] received %d columns in result from `%s` query on %s backend (expected just one)",
							PQnfields(result), sql, BACKENDS[i].endpoint);
					BACKENDS[i].ok = BACKEND_IS_HALFUP;
					PQclear(result);
					break;
				}

				val = PQgetvalue(result, 0, 0);
				pgr_logf(stderr, LOG_INFO, "backend %s returned '%s' for `%s`",
						BACKENDS[i].endpoint, val, sql);
				rc = xlog(val, &BACKENDS[i].pos);
				if (rc != 0) {
					PQclear(result);
					break;
				}

				/* keep track of our master position for lag calculations */
				if (BACKENDS[i].role == BACKEND_ROLE_MASTER) {
					master_pos = BACKENDS[i].pos;
				}

				BACKENDS[i].ok = BACKEND_IS_OK;
				PQclear(result);
				break;

			default:
				pgr_logf(stderr, LOG_ERR, "[watcher] unhandled connection status %d from backend %s",
						rc, BACKENDS[i].endpoint);
				PQclear(result);
				break;
			}

			PQfinish(conn);

			/* FIXME: look at using PQstartConnect() with a select loop */
		}

		/* now, loop over the backends and update them with our findings */
		wrlock(&c->lock, "context", 0);

		/* if num_backends changed, we may have serviced a configuration reload
		   in another thread while we were connectint to backends in the previous
		   loop;  it's best to just drop everything and let the next iteration
		   of the loop (without an intervening sleep) fire. */
		if (c->num_backends != NUM_BACKENDS) {
			pgr_logf(stderr, LOG_INFO, "[watcher] detected a change in the number of backends "
					"that probably occurred while we were checking backend health.");
			pgr_logf(stderr, LOG_INFO, "[watcher] ignoring this round of results in favor of an "
					"(immedite) next iteration");
			unlock(&c->lock, "context", 0);
			continue;
		}

		c->ok_backends = 0;
		for (i = 0; i < NUM_BACKENDS; i++) {
			wrlock(&c->backends[i].lock, "backend", i);

			/* we have to check serial again, in case we serviced a less invasive
			   configuration reload (in which case we may have changed ports,
			   or otherwise rendered our previous test for this backend non-viable.) */
			if (BACKENDS[i].serial != c->backends[i].serial) {
				pgr_logf(stderr, LOG_ERR, "[watcher] serial mismatch detected while we were "
						"trying to update the other threads with our findings...");
				pgr_logf(stderr, LOG_ERR, "[watcher] skipping backend/%d updates for now "
						"(hopefully things will have settled down on the next iteration)");

				unlock(&c->backends[i].lock, "backend", i);
				continue;
			}

			if (BACKENDS[i].ok == BACKEND_IS_OK) {
				c->ok_backends++;
			}
			c->backends[i].status = BACKENDS[i].ok;
			c->backends[i].role = BACKENDS[i].role;
			c->backends[i].health.lag = master_pos - BACKENDS[i].pos;
			pgr_logf(stderr, LOG_INFO, "[watcher] updated %s (%d) backend/%d with status %d (%s) and lag %d (%d/%d)",
					pgr_backend_role(c->backends[i].role), c->backends[i].role,
					i, c->backends[i].status, pgr_backend_status(c->backends[i].status),
					c->backends[i].health.lag, BACKENDS[i].pos, master_pos);

			unlock(&c->backends[i].lock, "backend", i);
		}

		/* save this for later; we look it up every time because it could
		   have changed due to a configuration reload since the last time we
		   were awake. */
		sleep_for = c->health.interval;

		unlock(&c->lock, "context", 0);

		pgr_debugf("sleeping for %d seconds", sleep_for);
		sleep(sleep_for);
	}

	return NULL;
}

int pgr_watcher(CONTEXT *c, pthread_t *tid)
{
	int rc = pthread_create(tid, NULL, do_watcher, c);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[watcher] failed to spin up: %s (errno %d)",
				strerror(errno), errno);
		return 1;
	}

	pgr_logf(stderr, LOG_INFO, "[watcher] spinning up [tid=%i]", *tid);
	return 0;
}
