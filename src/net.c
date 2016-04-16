#include "pgrouter.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int getport(const char *ep)
{
	errno = EINVAL;

	const char *p;
	for (p = ep; *p && *p != ':'; p++)
		;

	if (*p != ':') {
		return -1;
	}

	int port = 0;
	for (p++; *p; p++) {
		if (!isdigit(*p)) {
			return -1;
		}
		port = port * 10 + (*p - '0');
	}

	if (port > 0xffff) {
		return -1;
	}

	return port;
}

char* gethost(const char *ep)
{
	const char *p;
	for (p = ep; *p != ':'; p++)
		;

	char *host = calloc(p - ep + 1, sizeof(char));
	if (host) {
		strncpy(host, ep, p - ep);
	}
	return host;
}

static int bind_and_listen(const char *ep, struct sockaddr* sa, int fd, int backlog)
{
	int rc;
	int ena = 1;
	rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &ena, sizeof(ena));
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "failed to set SO_REUSEADDR on [%s]: %s (errno %d)",
				ep, strerror(errno), errno);
		pgr_logf(stderr, LOG_ERR, "(continuing, but bind may fail...)");
	}

	if (sa->sa_family == AF_INET6) {
		ena = 1;
		rc = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &ena, sizeof(ena));
		if (rc != 0) {
			pgr_logf(stderr, LOG_ERR, "failed to set IPV6_V6ONLY on [%s]: %s (errno %d)",
					ep, strerror(errno), errno);
			pgr_logf(stderr, LOG_ERR, "(continuing, but bind may fail...)");
		}
	}

	switch (sa->sa_family) {
	case AF_INET:  rc = bind(fd, sa, sizeof(struct sockaddr_in));  break;
	case AF_INET6: rc = bind(fd, sa, sizeof(struct sockaddr_in6)); break;
	default:
		pgr_logf(stderr, LOG_ERR, "unrecognized address family (%d) in call to bind_and_listen()",
				sa->sa_family);
		pgr_abort(ABORT_NET);
	}
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "failed to bind socket to [%s]: %s (errno %d)",
				ep, strerror(errno), errno);
		close(fd);
		return -1;
	}

	pgr_debugf("attempting to listen with a backlog of %d", backlog);
	rc = listen(fd, backlog);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "failed to listen on [%s]: %s (errno %d)",
				ep, strerror(errno), errno);
		close(fd);
		return -1;
	}

	pgr_logf(stderr, LOG_INFO, "listening on %s (fd %d)", ep, fd);
	return fd;
}

int pgr_listen4(const char *ep, int backlog)
{
	int rc, fd, port;
	char *host;

	port = getport(ep);
	host = gethost(ep);
	if (port < 0 || !host) {
		free(host);
		return -1;
	}

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port   = htons(port);

	if (strcmp(host, "*") == 0) {
		sa.sin_addr.s_addr  = INADDR_ANY;

	} else {
		rc = inet_pton(AF_INET, host, &sa.sin_addr);
		if (rc != 1) {
			if (rc < 0 && errno == EAFNOSUPPORT) {
				pgr_debugf("ipv4 address family not supported");
			} else {
				pgr_debugf("'%s' is not an ipv4 address", host);
			}
			free(host);
			return -1;
		}
	}
	free(host);
	host = NULL;

	fd = socket(sa.sin_family, SOCK_STREAM, 0);
	if (fd < 0) {
		pgr_logf(stderr, LOG_ERR, "failed to create an ipv4 socket for [%s]: %s (errno %d)",
				ep, strerror(errno), errno);
		return -1;
	}

	pgr_debugf("binding / listening on fd %d", fd);
	return bind_and_listen(ep, (struct sockaddr*)(&sa), fd, backlog);
}

int pgr_listen6(const char *ep, int backlog)
{
	int rc, fd, port;
	char *host;

	port = getport(ep);
	host = gethost(ep);
	if (port < 0 || !host) {
		free(host);
		return -1;
	}

	struct sockaddr_in6 sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin6_family = AF_INET6;
	sa.sin6_port   = htons(port);

	if (strcmp(host, "*") == 0) {
		sa.sin6_addr = in6addr_any;

	} else {
		rc = inet_pton(AF_INET6, host, &sa.sin6_addr);
		if (rc != 1) {
			if (rc < 0 && errno == EAFNOSUPPORT) {
				pgr_debugf("ipv6 address family not supported");
			} else {
				pgr_debugf("'%s' is not an ipv6 address", host);
			}
			free(host);
			return -1;
		}
	}
	free(host);
	host = NULL;

	fd = socket(sa.sin6_family, SOCK_STREAM, 0);
	if (fd < 0) {
		pgr_logf(stderr, LOG_ERR, "failed to create an ipv6 socket for [%s]: %s (errno %d)",
				ep, strerror(errno), errno);
		return -1;
	}

	pgr_debugf("binding / listening on fd %d", fd);
	return bind_and_listen(ep, (struct sockaddr*)(&sa), fd, backlog);
}
