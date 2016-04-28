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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>

#define SUBSYS "super"
#include "locks.inc.c"

#define DEFAULT_CONFIG_FILE "/etc/pgrouter.conf"

typedef struct {
	pthread_t  watcher;  /* watcher thread id       */
	pthread_t  monitor;  /* monitor thread id       */
	pthread_t *workers;  /* worker thread ids       */
	int        n;        /* how many worker threads */
} THREADSET;

static void do_shutdown(THREADSET *threads)
{
	int i;
	void *ret;

	/* first we cancel */
	pthread_cancel(threads->watcher);
	pthread_cancel(threads->monitor);
	for (i = 0; i < threads->n; i++) {
		pthread_cancel(threads->workers[i]);
	}

	/* then we join */
	pthread_join(threads->watcher, &ret);
	pthread_join(threads->monitor, &ret);

	for (i = 0; i < threads->n; i++) {
		pthread_join(threads->workers[i], &ret);
	}
}

static void inform_parent(int fd, const char *fmt, ...)
{
	ssize_t n;
	char error[8192];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(error, sizeof(error), fmt, ap);
	va_end(ap);

	n = write(fd, error, strlen(error));
	if (n < 0) {
		perror("failed to inform parent of our error condition");
	}
	if (n < strlen(error)) {
		fprintf(stderr, "child->parent inform - only wrote %li of %li bytes\n",
			n, strlen(error));
	}
}

static void daemonize(const char *pidfile, const char *user, const char *group)
{
	int rc, fd = -1;
	size_t n;

	umask(0);
	if (pidfile) {
		fd = open(pidfile, O_RDWR|O_CREAT, S_IRUSR|S_IWUSR);
		if (fd == -1) {
			perror(pidfile);
			exit(2);
		}
	}

	errno=0;
	struct passwd *pw = getpwnam(user);
	if (!pw) {
		fprintf(stderr, "Failed to look up user '%s': %s\n",
				user, (errno == 0 ? "user not found" : strerror(errno)));
		exit(2);
	}

	errno = 0;
	struct group *gr = getgrnam(group);
	if (!gr) {
		fprintf(stderr, "Failed to look up group '%s': %s\n",
				group, (errno == 0 ? "group not found" : strerror(errno)));
		exit(2);
	}

	/* chdir to fs root to avoid tying up mountpoints */
	rc = chdir("/");
	if (rc != 0) {
		fprintf(stderr, "Failed to change directory to /: %s\n",
				strerror(errno));
		exit(2);
	}

	/* child -> parent error communication pipe */
	int pfds[2];
	rc = pipe(pfds);
	if (rc != 0) {
		fprintf(stderr, "Failed to create communication pipe: %s\n",
				strerror(errno));
		exit(2);
	}

	/* fork */
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "Failed to fork child process: %s\n",
				strerror(errno));
		exit(2);
	}

	if (pid > 0) {
		close(pfds[1]);
		char buf[8192];
		while ( (n = read(pfds[0], buf, 8192)) > 0) {
			buf[n] = '\0';
			fprintf(stderr, "%s", buf);
		}
		exit(0);
	}
	close(pfds[0]);
	char error[8192];

	if (pidfile) {
		struct flock lock;
		size_t n;

		lock.l_type   = F_WRLCK;
		lock.l_whence = SEEK_SET;
		lock.l_start  = 0;
		lock.l_len    = 0; /* whole file */

		rc = fcntl(fd, F_SETLK, &lock);
		if (rc == -1) {
			snprintf(error, 8192, "Failed to acquire lock on %s.%s\n",
					pidfile,
					(errno == EACCES || errno == EAGAIN
						? "  Is another copy running?"
						: strerror(errno)));
			n = write(pfds[1], error, strlen(error));
			if (n < 0)
				perror("failed to inform parent of our error condition");
			if (n < strlen(error))
				fprintf(stderr, "child->parent inform - only wrote %li of %li bytes\n",
					(long)n, (long)strlen(error));
			exit(2);
		}
	}

	/* leave session group, lose the controlling term */
	rc = (int)setsid();
	if (rc == -1) {
		inform_parent(pfds[1], "Failed to drop controlling terminal: %s\n",
				strerror(errno));
		exit(2);
	}

	if (pidfile) {
		/* write the pid file */
		char buf[8];
		snprintf(buf, 8, "%i\n", getpid());
		n = write(fd, buf, strlen(buf));
		if (n < 0)
			perror("failed to write PID to pidfile");
		if (n < strlen(buf))
			fprintf(stderr, "only wrote %li of %li bytes to pidfile\n",
				(long)n, (long)strlen(error));
		fsync(fd);

		if (getuid() == 0) {
			/* chmod the pidfile, so it can be removed */
			rc = fchown(fd, pw->pw_uid, gr->gr_gid);
			if (rc != 0) {
				inform_parent(pfds[1], "Failed to change user/groupd ownership of pidfile %s: %s\n",
						pidfile, strerror(errno));
				unlink(pidfile);
				exit(2);
			}
		}
	}

	if (getuid() == 0) {
		/* set UID/GID */
		if (gr->gr_gid != getgid()) {
			rc = setgid(gr->gr_gid);
			if (rc != 0) {
				inform_parent(pfds[1], "Failed to switch to group '%s': %s\n",
						group, strerror(errno));
				unlink(pidfile);
				exit(2);
			}
		}
		if (pw->pw_uid != getuid()) {
			rc = setuid(pw->pw_uid);
			if (rc != 0) {
				inform_parent(pfds[1], "Failed to switch to user '%s': %s\n",
						user, strerror(errno));
				unlink(pidfile);
				exit(2);
			}
		}
	}

	/* redirect standard IO streams to/from /dev/null */
	if (!freopen("/dev/null", "r", stdin))
		perror("Failed to reopen stdin </dev/null");
	if (!freopen("/dev/null", "w", stdout))
		perror("Failed to reopen stdout >/dev/null");
	if (!freopen("/dev/null", "w", stderr))
		perror("Failed to reopen stderr >/dev/null");
	close(pfds[1]);
}

int main(int argc, char **argv)
{
	char *config = strdup(DEFAULT_CONFIG_FILE);
	int verbose = 0;
	int foreground = 0;

	int opt, idx = 0;
	const char *short_opts = "hC:Fv";
	struct option long_opts[] = {
		{ "help",             no_argument, NULL, 'h' },
		{ "config",     required_argument, NULL, 'C' },
		{ "foreground",       no_argument, NULL, 'F' },
		{ "verbose",          no_argument, NULL, 'v' },
		{ 0, 0, 0, 0 }
	};
	while ((opt = getopt_long(argc, argv, short_opts, long_opts, &idx)) != -1) {
		switch (opt) {
		case '?':
		case 'h':
			fprintf(stderr, "USAGE: %s [-hCFvvv]\n\n", argv[0]);
			fprintf(stderr, "  -h, --help         Show this help screen.\n");
			fprintf(stderr, "  -C, --config       Path to alternate configuration file.\n");
			fprintf(stderr, "                     (defaults to " DEFAULT_CONFIG_FILE ")\n");
			fprintf(stderr, "  -F, --foreground   Don't daemonize into the background.\n");
			fprintf(stderr, "  -v, --verbose      Increase log level  Can be used more than once.\n");
			fprintf(stderr, "                       -v   prints internal errors as they happen.\n");
			fprintf(stderr, "                       -vv  prints diagnostics for troubleshooting.\n");
			fprintf(stderr, "                       -vvv prints info only a developer could love.\n");
			return 0;

		case 'C':
			free(config);
			config = strdup(optarg);
			break;

		case 'F':
			foreground = 1;
			break;

		case 'v':
			verbose++;
			break;

		default:
			fprintf(stderr, "USAGE: %s [-hCFvvv]\n\n", argv[0]);
			return 1;
		}
	}
	switch (verbose) {
	case  0: break;
	case  1: pgr_logger(LOG_ERR);   break;
	case  2: pgr_logger(LOG_INFO);  break;
	default: pgr_logger(LOG_DEBUG); break;
	}

	pgr_logf(stderr, LOG_INFO, "pgrouter starting up");

	CONTEXT c;
	memset(&c, 0, sizeof(c));
	if (pgr_configure(&c, config, 0) != 0) {
		pgr_logf(stderr, LOG_ERR, "failed to load configuration from %s: %s (errno %d)",
				config, strerror(errno), errno);
		return 2;
	}

	int rc = pgr_context(&c);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "failed to initialize a new context: %s (errno %d)",
				strerror(errno), errno);
		return 3;
	}
	rc = pgr_authdb(&c, 0);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "failed to initialize authdb: %s (errno %d)",
				strerror(errno), errno);
		return 3;
	}

	if (!foreground) {
		daemonize(c.startup.pidfile, c.startup.user, c.startup.group);
	}

	pgr_logf(stderr, LOG_INFO, "[super] binding frontend to %s", c.startup.frontend);
	c.frontend4 = pgr_listen4(c.startup.frontend, FRONTEND_BACKLOG);
	c.frontend6 = pgr_listen6(c.startup.frontend, FRONTEND_BACKLOG);
	if (c.frontend4 < 0 && c.frontend6 < 0) {
		pgr_abort(ABORT_NET);
	}

	pgr_logf(stderr, LOG_INFO, "[super] binding monitor to %s", c.startup.monitor);
	c.monitor4 = pgr_listen4(c.startup.monitor, MONITOR_BACKLOG);
	c.monitor6 = pgr_listen6(c.startup.monitor, MONITOR_BACKLOG);
	if (c.monitor4 < 0 && c.monitor6 < 0) {
		pgr_abort(ABORT_NET);
	}

	THREADSET threads;
	threads.n = c.workers;
	threads.workers = calloc(threads.n, sizeof(pthread_t));
	if (!threads.workers) {
		pgr_abort(ABORT_MEMFAIL);
	}

	sigset_t allsigs;
	sigfillset(&allsigs);
	rc = pthread_sigmask(SIG_BLOCK, &allsigs, NULL);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[super] failed to block signals in master thread");
		return 4;
	}

	pgr_logf(stderr, LOG_INFO, "[super] spinning up WATCHER thread");
	rc = pgr_watcher(&c, &threads.watcher);
	if (rc != 0) {
		return 5;
	}

	pgr_logf(stderr, LOG_INFO, "[super] spinning up MONITOR thread");
	rc = pgr_monitor(&c, &threads.monitor);
	if (rc != 0) {
		return 6;
	}

	int i;
	for (i = 0; i < threads.n; i++) {
		pgr_logf(stderr, LOG_INFO, "[super] spinning up WORKER thread #%d", i+1);
		rc = pgr_worker(&c, &threads.workers[i]);
		if (rc != 0) {
			return 7;
		}
	}

	/* supervisor main loop (mostly signal handling) */
	for (;;) {
		int sig;
		sigset_t signals;

		sigemptyset(&signals);
		sigaddset(&signals, SIGTERM);
		sigaddset(&signals, SIGINT);
		sigaddset(&signals, SIGQUIT);
		sigaddset(&signals, SIGHUP);

		pgr_debugf("waiting for a signal...");
		rc = sigwait(&signals, &sig);
		if (rc < 0) {
			pgr_logf(stderr, LOG_ERR, "[super] errored while waiting for signals: %s (errno %d)",
					strerror(errno), errno);
			if (errno != EINTR) {
				break;
			}
		}

		switch (sig) {
		case SIGTERM:
			pgr_logf(stderr, LOG_INFO, "[super] caught SIGTERM (%d)", sig);
			pgr_logf(stderr, LOG_INFO, "pgrouter shutting down");
			do_shutdown(&threads);
			free(threads.workers);
			free(config);
			pgr_deconfigure(&c);
			return 1;

		case SIGINT:
			pgr_logf(stderr, LOG_INFO, "[super] caught SIGINT (%d)", sig);
			pgr_logf(stderr, LOG_INFO, "pgrouter shutting down");
			do_shutdown(&threads);
			free(threads.workers);
			free(config);
			pgr_deconfigure(&c);
			return 2;

		case SIGQUIT:
			pgr_logf(stderr, LOG_INFO, "[super] caught SIGQUIT (%d)", sig);
			pgr_logf(stderr, LOG_INFO, "pgrouter shutting down");
			do_shutdown(&threads);
			free(threads.workers);
			pgr_deconfigure(&c);
			return 3;

		case SIGHUP:
			pgr_logf(stderr, LOG_INFO, "[super] caught SIGHUP (%d)", sig);

			wrlock(&c.lock, "context", 0);
			rc = pgr_configure(&c, config, 1);
			if (rc != 0) {
				pgr_logf(stderr, LOG_ERR, "[super] RELOAD FAILED");
				unlock(&c.lock, "context", 0);
				break;
			}
			unlock(&c.lock, "context", 0);
			break;
		}
	}

	pgr_logf(stderr, LOG_INFO, "pgrouter shutting down abnormally...");
	return 0;
}
