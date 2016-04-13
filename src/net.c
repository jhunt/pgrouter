#include "pgrouter.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int getport(const char *ep)
{
	const char *p;
	for (p = ep; *p && *p != ':'; p++)
		;

	if (*p != ':') {
		/* FIXME: need an errno here */
		return -1;
	}

	int port = 0;
	for (p++; *p; p++) {
		if (!isdigit(*p)) {
			/* FIXME: need an errno here */
			return -1;
		}
		port = port * 10 + (*p - '0');
	}

	if (port > 0xffff) {
		/* FIXME: need an errno here */
		return -1;
	}

	return port;
}

static in_addr_t getipv4(const char *ep)
{
	const char *p;
	for (p = ep; *p != ':'; p++)
		;

	char *host = calloc(p - ep + 1, sizeof(char));
	if (!host) {
		pgr_logf(stderr, LOG_ERR, "failed to parse endpoint '%s': %s (errno %d)",
				ep, strerror(errno), errno);
		pgr_abort(ABORT_MEMFAIL);
	}

	strncpy(host, ep, p - ep);
	/* FIXME: use getifadds to enumerate local addresses */
	free(host);
	return INADDR_ANY;
}

int pgr_listen(const char *ep, int backlog)
{
	int rc, fd;
	struct sockaddr_in sa;
	int port;

	pgr_logf(stderr, LOG_DEBUG, "parsing '%s' to get interface/address and port", ep);
	port = getport(ep);
	if (port < 0) {
		return -1;
	}

	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = getipv4(ep);

	pgr_logf(stderr, LOG_DEBUG, "creating a datagram socket");
	fd = socket(sa.sin_family, SOCK_STREAM, 0);
	if (fd < 0) {
		pgr_logf(stderr, LOG_ERR, "failed to create socket for [%s]: %s (errno %d)",
				ep, strerror(errno), errno);
		return -1;
	}

	pgr_logf(stderr, LOG_DEBUG, "setting SO_REUSEADDR socket option (non-vital)");
	int ena = 1;
	rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &ena, sizeof(ena));
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "failed to set SO_REUSEADDR on [%s]: %s (errno %d)",
				ep, strerror(errno), errno);
		pgr_logf(stderr, LOG_ERR, "(continuing, but bind may fail...)");
	}

	pgr_logf(stderr, LOG_DEBUG, "attempting to bind to %s", ep);
	rc = bind(fd, (struct sockaddr*)(&sa), sizeof(sa));
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "failed to bind socket to [%s]: %s (errno %d)",
				ep, strerror(errno), errno);
		close(fd);
		return -1;
	}

	pgr_logf(stderr, LOG_DEBUG, "attempting to listen with a backlog of %d", backlog);
	rc = listen(fd, backlog);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "failed to listen on [%s]: %s (errno %d)",
				ep, strerror(errno), errno);
		close(fd);
		return -1;
	}

	pgr_logf(stderr, LOG_INFO, "listening on %s", ep);
	return fd;
}
