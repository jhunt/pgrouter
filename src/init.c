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
