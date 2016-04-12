#include "pgrouter.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>

static int parse_host_port(const char *s, char **host, int *port)
{
	const char *p;
	for (p = s; *p && *p != ':'; p++)
		;
	*host = calloc(p - s + 1, sizeof(char));
	if (!*host) {
		return 1;
	}

	strncpy(*host, s, p - s);
	if (*p == ':') {
		int i = 0;
		for (p++; *p; p++) {
			if (!isdigit(*p)) {
				free(*host);
				*host = NULL;
				return 1;
			}

			i = i * 10 + (*p - '0');
		}
		if (i > 0xffff) {
			free(*host);
			*host = NULL;
			return 1;
		}
		*port = i;
	}

	return 0;
}

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

	if (!c->startup.listen) {
		c->startup.listen = strdup(DEFAULT_LISTEN_BIND);
	}
	rc = parse_host_port(c->startup.listen, &c->listen_host, &c->listen_port);
	if (rc != 0) {
		return rc;
	}

	if (!c->startup.monitor) {
		c->startup.monitor = strdup(DEFAULT_MONITOR_BIND);
	}
	rc = parse_host_port(c->startup.monitor, &c->monitor_host, &c->monitor_port);
	if (rc != 0) {
		return rc;
	}

	return 0;
}
