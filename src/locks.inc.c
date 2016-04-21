#include <pthread.h>
#ifndef SUBSYS
#define SUBSYS "UNKNOWN-SUBSYSTEM"
#endif

static void rdlock(pthread_rwlock_t *l, const char *what, int idx)
{
#ifdef DEBUG_LOCKS
	pgr_debugf("[" SUBSYS "] acquiring %s/%d read lock %p", what, idx, l);
#endif
	int rc = pthread_rwlock_rdlock(l);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[" SUBSYS "] failed to acquire %s/%d read lock: %s (errno %d)",
				what, idx, strerror(rc), rc);
		pgr_abort(ABORT_LOCK);
	}
}

static void wrlock(pthread_rwlock_t *l, const char *what, int idx)
{
#ifdef DEBUG_LOCKS
	pgr_debugf("[" SUBSYS "] acquiring %s/%d write lock %p", what, idx, l);
#endif
	int rc = pthread_rwlock_wrlock(l);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[" SUBSYS "] failed to acquire %s/%d write lock: %s (errno %d)",
				what, idx, strerror(rc), rc);
		pgr_abort(ABORT_LOCK);
	}
}

static void unlock(pthread_rwlock_t *l, const char *what, int idx)
{
#ifdef DEBUG_LOCKS
	pgr_debugf("[" SUBSYS "] releasing %s/%d lock %p", what, idx, l);
#endif
	int rc = pthread_rwlock_unlock(l);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[" SUBSYS "] failed to release the %s/%d lock: %s (errno %d)",
				what, idx, strerror(rc), rc);
		pgr_abort(ABORT_LOCK);
	}
}
