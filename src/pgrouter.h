#ifndef PGROUTER_H
#define PGROUTER_H

#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>

/* Status of backends */
#define BACKEND_IS_OK        0  /* backend is up and running    */
#define BACKEND_IS_STARTING  1  /* backend is still connecting  */
#define BACKEND_IS_FAILED    2  /* backend won't connect        */

/* SSL/TLS behaviors */
#define BACKEND_TLS_OFF      0  /* don't do SSL/TLS             */
#define BACKEND_TLS_VERIFY   1  /* do SSL/TLS; verify certs.    */
#define BACKEND_TLS_NOVERIFY 2  /* do SSL/TLS; skip verif.      */

typedef unsigned long long int lag_t;

typedef struct {
	pthread_rwlock_t rwlock;    /* read/write lock for sync.    */
	char *hostname;             /* hostname/ip to connect to    */
	int port;                   /* port to connect to           */

	int tls;                    /* a BACKEND_TLS_* constant     */

	int master;                 /* 1 = master, 0 = slave        */
	int status;                 /* a BACKEND_IS_* constant      */
	int weight;                 /* weight, range [0, RAND_MAX]  */

	struct {
		const char *database;   /* database to connect to       */
		const char *username;   /* username to connect with     */
		const char *password;   /* password to auth. with       */

		lag_t lag;              /* replication lag, in bytes    */
		lag_t threshold;        /* threshold for lag (bytes)    */
	} health;
} BACKEND;

typedef struct {
	pthread_rwlock_t rwlock;    /* read/write lock for sync.    */

	int workers;                /* how many WORKER threads      */
	int loglevel;               /* what messages to log         */

	char *hbafile;              /* path to the HBA/ACL config.  */
	char *pidfile;              /* path to the daemon pidfile   */

	struct {
		int interval;           /* how often to check backends  */
		int timeout;            /* total timeout in seconds     */

		char *database;         /* database to connect to       */
		char *username;         /* username to connect with     */
		char *password;         /* password to auth. with       */
	} health;

	struct {
		char *listen_host;      /* host/ip to bind              */
		int   listen_port;      /* port to bind                 */

		char *monitor_host;     /* host/ip to bind              */
		int   monitor_port;     /* port to bind                 */

		char *tls_ciphers;      /* SSL/TLS ciphers to advert.   */
		char *tls_certfile;     /* path to SSL/TLS certificate  */
		char *tls_keyfile;      /* path to SSL/TLS key          */

		char *user;             /* user to run as               */
		char *group;            /* group to run as              */
		int daemonize;          /* to daemonize or not          */
	} startup;

	int num_backends;           /* how many backends are there? */
	BACKEND *backends;          /* the backends -- epic         */
} CONTEXT;

/* memory allocation subroutines */
void *pgr_alloc(size_t bytes);
#define pgr_make(t) (pgr_alloc(sizeof(t)))
void pgr_xfree(void **p);
#define pgr_free(p) (pgr_xfree(&(p)))

/* configuration subroutines */
int pgr_configure(CONTEXT *c, FILE *io, int reload);

/* logging subroutines */
void pgr_logf(FILE *io, int level, const char *fmt, ...);
void pgr_vlogf(FILE *io, int level, const char *fmt, va_list ap);

#endif
