#include "pgrouter.h"
#include <stdlib.h>
#include <string.h>
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

		c->backends[i].status = BACKEND_IS_STARTING;
	}

	if (!c->startup.frontend) {
		c->startup.frontend = strdup(DEFAULT_FRONTEND_BIND);
	}
	if (!c->startup.monitor) {
		c->startup.monitor = strdup(DEFAULT_MONITOR_BIND);
	}

	return 0;
}
