#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <syslog.h>
#include <time.h>

static int LOGLEVEL;

void pgr_logger(int level)
{
	switch (level) {
	case LOG_EMERG:
	case LOG_ALERT:
	case LOG_CRIT:
	case LOG_ERR:
		LOGLEVEL = LOG_ERR;
		break;

	case LOG_WARNING:
	case LOG_NOTICE:
	case LOG_INFO:
		LOGLEVEL = LOG_INFO;
		break;

	case LOG_DEBUG:
		LOGLEVEL = LOG_DEBUG;
		break;

	default:
		LOGLEVEL = LOG_ERR;
	}
}

static void _vlogf(FILE *io, const char *fmt, va_list ap)
{
	char *msg, tstamp[128];
	int n;
	struct tm t;
	time_t now = time(NULL);

	if (gmtime_r(&now, &t) == NULL) {
		/* FIXME: some kind of error? */
		return;
	}

	n = strftime(tstamp, sizeof(tstamp), "%c", &t);
	if (n < 0) {
		/* FIXME: some kind of error? */
		return;
	}

	n = vasprintf(&msg, fmt, ap);
	if (n < 0) {
		/* FIXME: some kind of error? */
		return;
	}

	fprintf(io, "[%s] %s\n", tstamp, msg);
	free(msg);
}

void pgr_logf(FILE *io, int level, const char *fmt, ...)
{
	if (level <= LOGLEVEL) {
		va_list ap;
		va_start(ap, fmt);
		_vlogf(io, fmt, ap);
		va_end(ap);
	}
}

void pgr_vlogf(FILE *io, int level, const char *fmt, va_list ap)
{
	if (level <= LOGLEVEL) {
		_vlogf(io, fmt, ap);
	}
}
