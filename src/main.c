#include "pgrouter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>

#define SUBSYS "super"
#include "locks.inc.c"

#define DEFAULT_CONFIG_FILE "/etc/pgrouter.conf"

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
	pgr_logf(stderr, LOG_ERR,  "NOTE: the -F option is currently ignored; "
	                           "pgrouter ALWAYS runs in the foreground!"); /* FIXME */

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

	struct {
		pthread_t  watcher;  /* watcher thread id       */
		pthread_t  monitor;  /* monitor thread id       */
		pthread_t *workers;  /* worker thread ids       */
		int        n;        /* how many worker threads */
	} threads;
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
			return 1;

		case SIGINT:
			pgr_logf(stderr, LOG_INFO, "[super] caught SIGINT (%d)", sig);
			pgr_logf(stderr, LOG_INFO, "pgrouter shutting down");
			return 2;

		case SIGQUIT:
			pgr_logf(stderr, LOG_INFO, "[super] caught SIGQUIT (%d)", sig);
			pgr_logf(stderr, LOG_INFO, "pgrouter shutting down");
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
