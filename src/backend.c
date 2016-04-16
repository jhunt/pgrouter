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
	int n;        /* number of viable backends (length of weights[]) */
	int *weights; /* cumulative weights */
	int cumulative;

	rc = rdlock(&c->lock, "context", 0);
	if (rc != 0) {
		return negative(rc);
	}

	/* go ahead and provision weights as if all backends are viable */
	weights = calloc(c->num_backends, sizeof(int));
	if (!weights) {
		unlock(&c->lock, "context", 0);
		pgr_logf(stderr, LOG_ERR, "failed to allocate memory to store cumulative backend weights");
		pgr_abort(ABORT_MEMFAIL);
	}

	n = 0;
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
			weights[n++] = c->backends[i].weight;
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

	if (n == 0) {
		pgr_logf(stderr, LOG_ERR, "[backend] no backends are viable!!");
		return -1;
	}

	int r = pgr_rand(0, cumulative);
	for (i = 0; i < n; i++) {
		if (weights[i] <= r) {
			return i;
		}
	}

	/* otherwise, return the first backend, because ... why not */
	pgr_logf(stderr, LOG_ERR, "unable to pick a random backend from our set of %d (rand [0,%d]) -- returning backend 0 by default",
			n, cumulative);
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
