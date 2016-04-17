#ifndef PGROUTER_H
#define PGROUTER_H

#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <syslog.h>

#define RAND_DEVICE "/dev/urandom"

/* Status of backends */
#define BACKEND_IS_OK        0  /* backend is up and running    */
#define BACKEND_IS_STARTING  1  /* backend is still connecting  */
#define BACKEND_IS_FAILED    2  /* backend won't connect        */
#define BACKEND_IS_HALFUP    3  /* backend is up, but broken    */

char* pgr_backend_status(int status);

/* Role the backend plays */
#define BACKEND_ROLE_UNKNOWN 0
#define BACKEND_ROLE_MASTER  1
#define BACKEND_ROLE_SLAVE   2

char* pgr_backend_role(int role);

/* SSL/TLS behaviors */
#define BACKEND_TLS_OFF      0  /* don't do SSL/TLS             */
#define BACKEND_TLS_VERIFY   1  /* do SSL/TLS; verify certs.    */
#define BACKEND_TLS_NOVERIFY 2  /* do SSL/TLS; skip verif.      */

/* Exit codes */
#define ABORT_UNKNOWN  1
#define ABORT_MEMFAIL  2
#define ABORT_LOCK     3
#define ABORT_NET      4
#define ABORT_SYSCALL  5
#define ABORT_RANDFAIL 6
#define ABORT_UNIMPL   7

/* Defaults */
#define DEFAULT_MONITOR_BIND  "127.0.0.1:14231"
#define DEFAULT_FRONTEND_BIND "*:5432"

/* Hard-coded values */
#define FRONTEND_BACKLOG 64
#define MONITOR_BACKLOG  64

typedef unsigned long long int lag_t;

typedef struct {
	pthread_rwlock_t lock;      /* read/write lock for sync.    */
	int serial;                 /* increment on config reload.  */

	char *hostname;             /* hostname/ip to connect to    */
	int port;                   /* port to connect to           */

	int tls;                    /* a BACKEND_TLS_* constant     */

	int role;                   /* a BACKEND_ROLE_* constant    */
	int status;                 /* a BACKEND_IS_* constant      */
	int weight;                 /* weight, range [0, RAND_MAX]  */

	struct {
		char *database;         /* database to connect to       */
		char *username;         /* username to connect with     */
		char *password;         /* password to auth. with       */

		lag_t lag;              /* replication lag, in bytes    */
		lag_t threshold;        /* threshold for lag (bytes)    */
	} health;
} BACKEND;

typedef struct {
	pthread_rwlock_t lock;      /* read/write lock for sync.    */

	int frontend4;              /* ipv4 pg frontend socket      */
	int frontend6;              /* ipv6 pg frontend socket      */
	int monitor4;               /* ipv4 monitoring socket       */
	int monitor6;               /* ipv6 monitoring socket       */

	int workers;                /* how many WORKER threads      */
	int loglevel;               /* what messages to log         */

	struct {
		int interval;           /* how often to check backends  */
		int timeout;            /* total timeout in seconds     */

		char *database;         /* database to connect to       */
		char *username;         /* username to connect with     */
		char *password;         /* password to auth. with       */
	} health;

	struct {
		char *frontend;         /* frontend bind endpoint       */
		char *monitor;          /* monitor bind endpoint        */

		char *hbafile;          /* path to the HBA/ACL config.  */
		char *pidfile;          /* path to the daemon pidfile   */

		char *tls_ciphers;      /* SSL/TLS ciphers to advert.   */
		char *tls_certfile;     /* path to SSL/TLS certificate  */
		char *tls_keyfile;      /* path to SSL/TLS key          */

		char *user;             /* user to run as               */
		char *group;            /* group to run as              */
		int daemonize;          /* to daemonize or not          */
	} startup;

	int ok_backends;            /* how many healthy backends?   */
	int num_backends;           /* how many *total* backends?   */
	BACKEND *backends;          /* the backends -- epic         */
} CONTEXT;

/* process control subroutines */
void pgr_abort(int code);

/* randomness subroutines */
int pgr_rand(int start, int end);
void pgr_srand(int seed);

/* configuration subroutines */
int pgr_configure(CONTEXT *c, const char *file, int reload);
int pgr_context(CONTEXT *c);

/* logging subroutines */
void pgr_logger(int level);
void pgr_logf(FILE *io, int level, const char *fmt, ...);
void pgr_vlogf(FILE *io, int level, const char *fmt, va_list ap);
void pgr_dlogf(FILE *io, int level, const char *file, int line, const char *fn, const char *fmt, ...);
void pgr_vdlogf(FILE *io, int level, const char *file, int line, const char *fn, const char *fmt, va_list ap);
#define pgr_debugf(...) pgr_dlogf(stderr, LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)

/* network subroutines */
int pgr_listen4(const char *ep, int backlog);
int pgr_listen6(const char *ep, int backlog);
int pgr_connect_ep(const char *ep, int timeout_ms);
int pgr_connect(const char *host, int port, int timeout_ms);
int pgr_sendn(int fd, const void *buf, size_t n);
int pgr_sendf(int fd, const char *fmt, ...);
int pgr_recvn(int fd, const void *buf, size_t n);

/* backend subroutines */
int pgr_pick_any(CONTEXT *c);
int pgr_pick_master(CONTEXT *c);

/* thread subroutines */
int pgr_watcher(CONTEXT *c, pthread_t* tid);
int pgr_monitor(CONTEXT *c, pthread_t* tid);
int pgr_worker(CONTEXT *c, pthread_t *tid);

#endif
