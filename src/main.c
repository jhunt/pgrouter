#include "pgrouter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>

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

	/*
	rc = init_context(&c);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "failed to initialize a new context: %s (errno %d)",
				strerror(errno), errno);
		return 3;
	}
	*/

	/* FIXME: spin a supervisor */
	/* FIXME: spin a watcher */
	pgr_watcher(&c);

	/* FIXME: spin a monitor */
	/* FIXME: spin the workers */

	printf("hello, pgrouter!\n");

	pgr_logf(stderr, LOG_INFO, "pgrouter shutting down");
	sleep(10);
	return 0;
}
