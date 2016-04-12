#include "pgrouter.h"
#include <pthread.h>

int pgr_context(CONTEXT *c)
{
	int rc = pthread_rwlock_init(&c->lock, NULL);
	if (rc != 0) {
		return rc;
	}

	int i;
	for (i = 0; i < c->num_backends; i++) {
		rc = pthread_rwlock_init(&c->backends[i].lock, NULL);
		if (rc != 0) {
			return rc;
		}
	}

	/* FIXME: parse out startup.listen into host/port pieces */
	/* FIXME: parse out startup.monitor into host/port pieces */

	return rc;
}
