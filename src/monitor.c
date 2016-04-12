#include "pgrouter.h"
#include <pthread.h>

static int rdlock(pthread_rwlock_t *l, const char *what, int idx)
{
	pgr_logf(stderr, LOG_DEBUG, "[monitor] acquiring %s/%d read lock %p", what, idx, l);
	int rc = pthread_rwlock_rdlock(l);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[monitor] failed to acquire %s/%d read lock: %s (errno %d)",
				what, idx, strerror(rc), rc);
		return rc;
	}
	return 0;
}

static int unlock(pthread_rwlock_t *l, const char *what, int idx)
{
	pgr_logf(stderr, LOG_DEBUG, "[monitor] releasing %s/%d read lock %p", what, idx, l);
	int rc = pthread_rwlock_unlock(l);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[monitor] failed to release the %s/%d read lock: %s (errno %d)",
				what, idx, strerror(rc), rc);
		return rc;
	}
	return 0;
}

static void* do_monitor(void *_c)
{
	/* FIXME: parse out our interface / port */
	/* FIXME: create a socket */
	/* FIXME: bind the socket to our interface(s) / port */
	/* FIXME: listen for incoming connections */
	/* FIXME: accept a connection */
		/* FIXME: lock the context for read */
		/* FIXME; dump global monitoring data */
			/* FIXME: lock each backend for read */
				/* FIXME: dump monitoring data for each backend */
			/* FIXME: release backend lock */
		/* FIXME: release context lock */
}

void pgr_monitor(CONTEXT *c)
{
	pthread_t tid;
	int rc = pthread_create(&tid, NULL, do_monitor, c);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[monitor] failed to spin up: %s (errno %d)",
				strerror(errno), errno);
		return;
	}

	pgr_logf(stderr, LOG_INFO, "[monitor] spinning up [tid=%i]", tid);
}
