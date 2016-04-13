#include "pgrouter.h"
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>

static int rdlock(pthread_rwlock_t *l, const char *what, int idx)
{
	pgr_logf(stderr, LOG_DEBUG, "[worker] acquiring %s/%d read lock %p", what, idx, l);
	int rc = pthread_rwlock_rdlock(l);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[worker] failed to acquire %s/%d read lock: %s (errno %d)",
				what, idx, strerror(rc), rc);
		return rc;
	}
	return 0;
}

static int wrlock(pthread_rwlock_t *l, const char *what, int idx)
{
	pgr_logf(stderr, LOG_DEBUG, "[worker] acquiring %s/%d write lock %p", what, idx, l);
	int rc = pthread_rwlock_wrlock(l);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[worker] failed to acquire %s/%d write lock: %s (errno %d)",
				what, idx, strerror(rc), rc);
		return rc;
	}
	return 0;
}

static int unlock(pthread_rwlock_t *l, const char *what, int idx)
{
	pgr_logf(stderr, LOG_DEBUG, "[worker] releasing %s/%d read lock %p", what, idx, l);
	int rc = pthread_rwlock_unlock(l);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[worker] failed to release the %s/%d read lock: %s (errno %d)",
				what, idx, strerror(rc), rc);
		return rc;
	}
	return 0;
}

static void* do_worker(void *_c)
{
	CONTEXT *c = (CONTEXT*)_c;
	int rc;

	for (;;) {
		/* FIXME: block on accept() */
		/* FIXME: acquire wrlock on CONTEXT to update #clients (++) */
		/* FIXME: handle the incoming client connection */
		/* FIXME: close the connection fd */
		/* FIXME: acquire wrlock on CONTEXT to update #clients (--) */
		sleep(5); /* FIXME: remove this sleep */
	}

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
