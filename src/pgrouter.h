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
#define ABORT_ABSURD   8

/* Message types */
#define MSG_STARTUP_MESSAGE 1
#define MSG_SSL_REQUEST     2
#define MSG_CANCEL_REQUEST  3

/* Defaults */
#define DEFAULT_MONITOR_BIND  "127.0.0.1:14231"
#define DEFAULT_FRONTEND_BIND "*:5432"

/* Hard-coded values */
#define FRONTEND_BACKLOG 64
#define MONITOR_BACKLOG  64

typedef unsigned long long int lag_t;

typedef struct {
	unsigned int  hi, lo;
	unsigned int  a, b, c, d;
	unsigned char buf[64];
	unsigned int  blk[16];
} MD5;

typedef struct {
	char type;     /* one of the MSG_* constants          */
	int length;    /* length of message payload (data+4)  */
	char *data;    /* the actual data, (len-4) octets     */

	/* auxiliary fields, based on type */
	struct {
		int code;
	} auth;

	struct {
		char *severity;   /* */
		char *sqlstate;   /* */
		char *message;    /* */
		char *details;    /* */
		char *hint;       /* */
	} error;
} MESSAGE;

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
		char *file;             /* path to authdb               */
		int num_entries;        /* how many entries are there?  */
		char **usernames;       /* username entries             */
		char **md5hashes;       /* md5(password + username)     */
	} authdb;

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

	int fe_conns;               /* how many connected clients?  */
	int be_conns;               /* how many backend conn.?      */

	int ok_backends;            /* how many healthy backends?   */
	int num_backends;           /* how many *total* backends?   */
	BACKEND *backends;          /* the backends -- epic         */
} CONTEXT;

typedef struct __param PARAM;
struct __param {
	char *name;
	char *value;
	struct __param *next;
};

typedef struct {
	CONTEXT *context;

	int index;
	int serial;

	char *hostname;
	int port;

	char *username;
	char *database;
	const char *pwhash;
	char salt[4];

	PARAM *params;

	int fd;
} CONNECTION;

/* process control subroutines */
void pgr_abort(int code);

/* randomness subroutines */
int pgr_rand(int start, int end);
void pgr_srand(int seed);

/* configuration subroutines */
int pgr_configure(CONTEXT *c, const char *file, int reload);
int pgr_authdb(CONTEXT *c, int reload);
int pgr_context(CONTEXT *c);

/* authentication subroutines */
const char* pgr_auth_find(CONTEXT *c, const char *username);

/* hashing subroutines */
void pgr_md5_init(MD5 *md5);
void pgr_md5_update(MD5 *md5, const void *data, size_t len);
void pgr_md5_raw(unsigned char dst[16], MD5 *md5);
void pgr_md5_hex(char dst[32], MD5 *md5);

/* logging subroutines */
void pgr_logger(int level);
void pgr_logf(FILE *io, int level, const char *fmt, ...);
void pgr_vlogf(FILE *io, int level, const char *fmt, va_list ap);
void pgr_dlogf(FILE *io, int level, const char *file, int line, const char *fn, const char *fmt, ...);
void pgr_vdlogf(FILE *io, int level, const char *file, int line, const char *fn, const char *fmt, va_list ap);
void pgr_hexdump(const void *buf, size_t len);
#define pgr_debugf(...) pgr_dlogf(stderr, LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)

/* network subroutines */
int pgr_listen4(const char *ep, int backlog);
int pgr_listen6(const char *ep, int backlog);
int pgr_connect_ep(const char *ep, int timeout_ms);
int pgr_connect(const char *host, int port, int timeout_ms);
int pgr_sendn(int fd, const void *buf, size_t n);
int pgr_sendf(int fd, const char *fmt, ...);
int pgr_recvn(int fd, const void *buf, size_t n);

/* connection subroutines */
void pgr_conn_init(CONTEXT *c, CONNECTION *dst);
void pgr_conn_frontend(CONNECTION *dst, int fd);
void pgr_conn_backend(CONNECTION *dst, BACKEND *b, int i);
int pgr_conn_copy(CONNECTION *dst, CONNECTION *src);
int pgr_conn_connect(CONNECTION *c);
int pgr_conn_accept(CONNECTION *c);

/* message (protocol) subroutines */
/* FIXME: need timeout variation of pgr_msg_recv */
int pgr_msg_recv(int fd, MESSAGE *m);
int pgr_msg_send(int fd, MESSAGE *m);
void pgr_msg_pack(MESSAGE *m);
void pgr_msg_clear(MESSAGE *m);

/* thread subroutines */
int pgr_watcher(CONTEXT *c, pthread_t* tid);
int pgr_monitor(CONTEXT *c, pthread_t* tid);
int pgr_worker(CONTEXT *c, pthread_t *tid);

#endif
