#include "pgrouter.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

static int negative(int x)
{
	return x > 0 ? -1 * x : x;
}

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

static char* gethost(const char *ep)
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

static int ipv4_hostport(struct sockaddr_in *sa, const char *host, int port)
{
	int rc;

	sa->sin_family = AF_INET;
	sa->sin_port   = htons(port);

	if (strcmp(host, "*") == 0) {
		sa->sin_addr.s_addr = INADDR_ANY;

	} else {
		rc = inet_pton(AF_INET, host, &sa->sin_addr);
		if (rc != 1) {
			if (rc < 0 && errno == EAFNOSUPPORT) {
				pgr_debugf("ipv4 address family not supported");
			} else {
				pgr_debugf("'%s' is not an ipv4 address", host);
			}
			return -1;
		}
	}
	return 0;
}

static int ipv4_endpoint(struct sockaddr_in *sa, const char *ep)
{
	int rc, port;
	char *host;

	port = getport(ep);
	host = gethost(ep);
	if (port < 0 || !host) {
		free(host);
		return -1;
	}

	rc = ipv4_hostport(sa, host, port);
	free(host);
	return rc;
}

static int ipv6_hostport(struct sockaddr_in6 *sa, const char *host, int port)
{
	int rc;

	sa->sin6_family = AF_INET6;
	sa->sin6_port   = htons(port);

	if (strcmp(host, "*") == 0) {
		sa->sin6_addr = in6addr_any;

	} else {
		rc = inet_pton(AF_INET6, host, &sa->sin6_addr);
		if (rc != 1) {
			if (rc < 0 && errno == EAFNOSUPPORT) {
				pgr_debugf("ipv6 address family not supported");
			} else {
				pgr_debugf("'%s' is not an ipv6 address", host);
			}
			return -1;
		}
	}
	return 0;
}

static int ipv6_endpoint(struct sockaddr_in6 *sa, const char *ep)
{
	int rc, port;
	char *host;

	port = getport(ep);
	host = gethost(ep);
	if (port < 0 || !host) {
		free(host);
		return -1;
	}

	rc = ipv6_hostport(sa, host, port);
	free(host);
	return 0;
}

int pgr_listen4(const char *ep, int backlog)
{
	int rc, fd;
	struct sockaddr_in sa;

	memset(&sa, 0, sizeof(sa));
	rc = ipv4_endpoint(&sa, ep);
	if (rc != 0) {
		return rc;
	}

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
	int rc, fd;
	struct sockaddr_in6 sa;
	memset(&sa, 0, sizeof(sa));
	rc = ipv6_endpoint(&sa, ep);
	if (rc != 0) {
		return rc;
	}

	fd = socket(sa.sin6_family, SOCK_STREAM, 0);
	if (fd < 0) {
		pgr_logf(stderr, LOG_ERR, "failed to create an ipv6 socket for [%s]: %s (errno %d)",
				ep, strerror(errno), errno);
		return -1;
	}

	pgr_debugf("binding / listening on fd %d", fd);
	return bind_and_listen(ep, (struct sockaddr*)(&sa), fd, backlog);
}

int pgr_connect(const char *host, int port, int timeout_ms)
{
	int rc, fd, type;
	struct sockaddr_in ipv4;
	struct sockaddr_in6 ipv6;
	memset(&ipv4, 0, sizeof(ipv4));
	memset(&ipv6, 0, sizeof(ipv6));

	rc = ipv4_hostport(&ipv4, host, port);
	if (rc == 0) {
		fd = socket(ipv4.sin_family, SOCK_STREAM, 0);
		type = 4;
	} else {
		rc = ipv6_hostport(&ipv6, host, port);
		if (rc != 0) {
			return -1;
		}
		fd = socket(ipv6.sin6_family, SOCK_STREAM, 0);
		type = 6;
	}
	if (fd < 0) {
		pgr_logf(stderr, LOG_ERR, "failed to create an ipv%d socket for host %s on port %d: %s (errno %d)",
				type, host, port, strerror(errno), errno);
		return -1;
	}

	switch (type) {
	case 4: rc = connect(fd, (struct sockaddr*)(&ipv4), sizeof(ipv4)); break;
	case 6: rc = connect(fd, (struct sockaddr*)(&ipv6), sizeof(ipv6)); break;
	default:
		pgr_logf(stderr, LOG_ERR, "unrecognized IP version %d", type);
		close(fd);
		return -1;
	}
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "failed to connect (ipv%d) to host %s on port %d: %s (errno %d)",
				type, host, port, strerror(errno), errno);
		close(fd);
		return negative(rc);
	}
	return fd;
}

int pgr_sendn(int fd, const void *buf, size_t n)
{
	ssize_t nwrit;
	const void *p = buf;
	pgr_hexdump(buf, n);
	while (n > 0) {
		nwrit = write(fd, p, n);
		if (nwrit <= 0) {
			pgr_debugf("failed to write to fd %d: %s (errno %d)",
					fd, strerror(errno), errno);
			return 1;
		}
		n -= nwrit;
		p += nwrit;
	}

	return 0;
}

int pgr_sendf(int fd, const char *fmt, ...)
{
	int rc, n;
	char *buf;
	va_list ap;

	va_start(ap, fmt);
	n = vasprintf(&buf, fmt, ap);
	va_end(ap);

	rc = pgr_sendn(fd, buf, n);
	free(buf);
	return rc;
}

int pgr_recvn(int fd, void *buf, size_t n)
{
	ssize_t nread;
	void *p = buf;
	while (n > 0) {
		nread = read(fd, p, n);
		if (nread <= 0) {
			pgr_debugf("failed to read from fd %d: %s (errno %d)",
					fd, strerror(errno), errno);
			return 1;
		}

		n -= nread;
		p += nread;
	}

	return 0;
}
