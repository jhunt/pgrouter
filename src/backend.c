#include "pgrouter.h"
#include <stdlib.h>

#define SUBSYS "backend"
#include "locks.inc.c"

static int negative(int x)
{
	return x > 0 ? -1 * x : x;
}

int pgr_pick_any(CONTEXT *c)
{
	int rc, i;
	int *weights;
	int cumulative;

	rc = rdlock(&c->lock, "context", 0);
	if (rc != 0) {
		return negative(rc);
	}

	weights = calloc(c->num_backends, sizeof(int));
	if (!weights) {
		unlock(&c->lock, "context", 0);
		pgr_logf(stderr, LOG_ERR, "failed to allocate memory to store cumulative backend weights");
		pgr_abort(ABORT_MEMFAIL);
	}

	cumulative = 0;
	for (i = 0; i < c->num_backends; i++) {
		rc = rdlock(&c->backends[i].lock, "backend", i);
		if (rc != 0) {
			unlock(&c->lock, "context", 0);
			return negative(rc);
		}

		/* check each backend for viability, store cumulative weight */
		if (c->backends[i].status == BACKEND_IS_OK &&
		    c->backends[i].health.lag < c->backends[i].health.threshold) {

			cumulative += c->backends[i].weight;
			weights[i] = cumulative;
		}

		rc = unlock(&c->backends[i].lock, "backend", i);
		if (rc != 0) {
			unlock(&c->lock, "context", 0);
			return negative(rc);
		}
	}

	rc = unlock(&c->lock, "context", 0);
	if (rc != 0) {
		return negative(rc);
	}

	if (cumulative == 0) {
		pgr_logf(stderr, LOG_ERR, "[backend] no backends are viable!!");
		return -1;
	}

	int r = pgr_rand(0, cumulative);
	pgr_debugf("picking backend using random value %d from (%d,%d)",
			r, 0, cumulative);
	for (i = 0; i < c->num_backends; i++) {
		pgr_debugf("checking backend %d (cumulative weight %d) against %d",
			i, weights[i], r);
		if (r <= weights[i]) {
			pgr_logf(stderr, LOG_INFO, "[backend] using backend %d", i);
			return i;
		}
	}

	/* otherwise, return the first backend, because ... why not */
	pgr_logf(stderr, LOG_ERR, "unable to pick a random backend from our set of %d (rand [0,%d]) -- returning backend 0 by default",
			c->num_backends, cumulative);
	return 0;
}

int pgr_pick_master(CONTEXT *c)
{
	int rc, i;

	rc = rdlock(&c->lock, "context", 0);
	if (rc != 0) {
		return negative(rc);
	}

	int master = -1;
	for (i = 0; i < c->num_backends; i++) {
		rc = rdlock(&c->backends[i].lock, "backend", i);
		if (rc != 0) {
			unlock(&c->lock, "context", 0);
			return negative(rc);
		}

		/* check each backend for viability, store cumulative weight */
		if (c->backends[i].role == BACKEND_ROLE_MASTER) {
			master = i;
		}

		rc = unlock(&c->lock, "backend", i);
		if (rc != 0) {
			unlock(&c->lock, "context", 0);
			return negative(rc);
		}
	}

	rc = unlock(&c->lock, "context", 0);
	if (rc != 0) {
		return negative(rc);
	}

	return master;
}
