#include "pgrouter.h"
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <libpq-fe.h>

typedef struct {
	int serial;         /* BACKEND.serial; used to detect changes      */
	int timeout;        /* health check connection timeout, in seconds */

	int ok;             /* is the backend is accepting connections?    */
	int master;         /* is the backend the write master?            */
	lag_t lag;          /* how far behind the master is this slave?    */

	char endpoint[256]; /* "<host>:<port>" string, for diagnostics     */
	char userdb[256];   /* "<user>@<database>" string, for diagnostics */
	char dsn[1024];     /* full PostgreSQL DSN for the health database */
} HEALTH;

static int NUM_BACKENDS; /* how many backends are there?               */
static HEALTH *BACKENDS; /* health information, cached for speed       */

static int rdlock(pthread_rwlock_t *l, const char *what, int idx)
{
	pgr_logf(stderr, LOG_DEBUG, "[watcher] acquiring %s/%d read lock %p", what, idx, l);
	int rc = pthread_rwlock_rdlock(l);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[watcher] failed to acquire %s/%d read lock: %s (errno %d)",
				what, idx, strerror(rc), rc);
		return rc;
	}
	return 0;
}

static int wrlock(pthread_rwlock_t *l, const char *what, int idx)
{
	pgr_logf(stderr, LOG_DEBUG, "[watcher] acquiring %s/%d write lock %p", what, idx, l);
	int rc = pthread_rwlock_wrlock(l);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[watcher] failed to acquire %s/%d write lock: %s (errno %d)",
				what, idx, strerror(rc), rc);
		return rc;
	}
	return 0;
}

static int unlock(pthread_rwlock_t *l, const char *what, int idx)
{
	pgr_logf(stderr, LOG_DEBUG, "[watcher] releasing %s/%d read lock %p", what, idx, l);
	int rc = pthread_rwlock_unlock(l);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[watcher] failed to release the %s/%d read lock: %s (errno %d)",
				what, idx, strerror(rc), rc);
		return rc;
	}
	return 0;
}

static void* do_watcher(void *_c)
{
	int sleep_for, n;
	CONTEXT *c = (CONTEXT*)_c;
	int rc;

	for (;;) {
		rc = rdlock(&c->lock, "context", 0);
		if (rc != 0) {
			pgr_abort(ABORT_LOCK);
		}

		/* if we added or removed backends as part of a configuration
		   reload, we need to reallocate the cached health information. */
		if (c->num_backends != NUM_BACKENDS) {
			pgr_logf(stderr, LOG_DEBUG, "[watcher] number of backends changed (old %d != new %d); "
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
			rc = rdlock(&c->backends[i].lock, "backend", i);
			if (rc != 0) {
				unlock(&c->lock, "context", 0);
				pgr_abort(ABORT_LOCK);
			}

			if (c->backends[i].serial != BACKENDS[i].serial) {
				pgr_logf(stderr, LOG_DEBUG, "[watcher] backend/%d cached serial %d != actual serial %d; updating cache entry",
						i, BACKENDS[i].serial, c->backends[i].serial);
				pgr_logf(stderr, LOG_INFO, "[watcher] updating backend/%d with (potential) new connection information", i);

				BACKENDS[i].timeout = c->health.timeout; /* FIXME: race condition-ish */
				pgr_logf(stderr, LOG_DEBUG, "[watcher] backend/%d: setting health check timeout to %ds",
						i, BACKENDS[i].timeout);

				/* endpoint = "host:port" */
				n = snprintf(BACKENDS[i].endpoint, sizeof(BACKENDS[i].endpoint),
						"%s:%d", c->backends[i].hostname, c->backends[i].port);
				pgr_logf(stderr, LOG_DEBUG, "[watcher] backend/%d: setting endpoint to '%s'",
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
				pgr_logf(stderr, LOG_DEBUG, "[watcher] backend/%d: setting user@db to '%s'",
						i, BACKENDS[i].userdb);
				if (n >= sizeof(BACKENDS[i].userdb)) {
					pgr_logf(stderr, LOG_ERR, "[watcher] re-cache of backend/%d user@db string failed; "
							"'%s@%s' is more than %d bytes long",
							i, c->backends[i].health.username, c->backends[i].health.database,
							sizeof(BACKENDS[i].userdb) - 1);
				}

				n = snprintf(BACKENDS[i].dsn, sizeof(BACKENDS[i].dsn),
						"host=%s port=%d user=%s password='%s' dbname=%s "
						"connect_timeout=%d application=pgrouter",
						c->backends[i].hostname, c->backends[i].port,
						c->backends[i].health.username, c->backends[i].health.password,
						c->backends[i].health.database, BACKENDS[i].timeout);
				pgr_logf(stderr, LOG_DEBUG, "[watcher] backend/%d: setting dsn to '%s'",
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
				pgr_logf(stderr, LOG_DEBUG, "[watcher] backend/%d: setting serial to %d",
						i, BACKENDS[i].serial);
			}

			rc = unlock(&c->backends[i].lock, "backend", i);
			if (rc != 0) {
				unlock(&c->lock, "context", 0);
				pgr_abort(ABORT_LOCK);
			}
		}
		rc = unlock(&c->lock, "context", 0);
		if (rc != 0) {
			pgr_abort(ABORT_LOCK);
		}

		/* now, loop over the backends and gather our health data */
		int ok = 0;
		for (i = 0; i < NUM_BACKENDS; i++) {
			BACKENDS[i].ok     = 0;
			BACKENDS[i].master = 0;
			BACKENDS[i].lag    = 0;

			pgr_logf(stderr, LOG_INFO, "[watcher] checking backend/%d %s (connecting as %s)",
					i, BACKENDS[i].endpoint, BACKENDS[i].userdb);
			pgr_logf(stderr, LOG_DEBUG, "[watcher] connecting with dsn '%s'", BACKENDS[i].dsn);

			PGconn *conn = PQconnectdb(BACKENDS[i].dsn);
			if (!conn) {
				pgr_logf(stderr, LOG_ERR, "[watcher] failed to allocate memory for connection to %s backend",
					BACKENDS[i].endpoint);
				pgr_abort(ABORT_MEMFAIL);
			}

			switch (rc = PQstatus(conn)) {
			case CONNECTION_BAD:
				pgr_logf(stderr, LOG_ERR, "[watcher] failed to connect to %s backend",
					BACKENDS[i].endpoint);
				break;

			case CONNECTION_OK:
				pgr_logf(stderr, LOG_INFO, "[watcher] connected to %s backend",
					BACKENDS[i].endpoint);

				ok++;
				BACKENDS[i].ok = 1;
				/* FIXME: determine if master or slave */
				/* FIXME: determine replication offset */
				break;

			default:
				pgr_logf(stderr, LOG_ERR, "[watcher] unhandled connection status %d from backend %s",
						rc, BACKENDS[i].endpoint);
				break;
			}
			/* FIXME: look at using PQstartConnect() with a select loop */
		}

		/* now, loop over the backends and update them with our findings */
		rc = wrlock(&c->lock, "context", 0);
		if (rc != 0) {
			pgr_abort(ABORT_LOCK);
		}

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

		c->ok_backends = ok;
		for (i = 0; i < NUM_BACKENDS; i++) {
			rc = wrlock(&c->backends[i].lock, "backend", i);
			if (rc != 0) {
				unlock(&c->lock, "context", 0);
				pgr_abort(ABORT_LOCK);
			}

			/* we have to check serial again, in case we serviced a less invasive
			   configuration reload (in which case we may have changed ports,
			   or otherwise rendered our previous test for this backend non-viable.) */
			if (BACKENDS[i].serial != c->backends[i].serial) {
				pgr_logf(stderr, LOG_ERR, "[watcher] serial mismatch detected while we were "
						"trying to update the other threads with our findings...");
				pgr_logf(stderr, LOG_ERR, "[watcher] skipping backend/%d updates for now "
						"(hopefully things will have settled down on the next iteration)");

				rc = unlock(&c->backends[i].lock, "backend", i);
				if (rc != 0) {
					unlock(&c->lock, "context", 0);
					pgr_abort(ABORT_LOCK);
				}

				continue;
			}

			c->backends[i].status = (BACKENDS[i].ok ? BACKEND_IS_OK : BACKEND_IS_FAILED);
			c->backends[i].health.lag = BACKENDS[i].lag;
			pgr_logf(stderr, LOG_INFO, "[watcher] updated backend/%d with status %d (%s) and lag %d",
					i, c->backends[i].status, (BACKENDS[i].ok ? "OK" : "FAILED"),
					c->backends[i].health.lag);

			rc = unlock(&c->backends[i].lock, "backend", i);
			if (rc != 0) {
				unlock(&c->lock, "context", 0);
				pgr_abort(ABORT_LOCK);
			}
		}

		/* save this for later; we look it up every time because it could
		   have changed due to a configuration reload since the last time we
		   were awake. */
		sleep_for = c->health.interval;

		rc = unlock(&c->lock, "context", 0);
		if (rc != 0) {
			pgr_abort(ABORT_LOCK);
		}

		pgr_logf(stderr, LOG_DEBUG, "[watcher] sleeping for %d seconds", sleep_for);
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
