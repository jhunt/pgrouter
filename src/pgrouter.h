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

#ifndef PGROUTER_H
#define PGROUTER_H

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
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
	int timeout;

	char *username;
	char *database;
	const char *pwhash;
	char salt[4];

	PARAM *params;

	int fd;
} CONNECTION;

#define MSG_BUFSIZ 16384

typedef struct {
	char buf[MSG_BUFSIZ];
	int free;    /* how many bytes are free in the buffer */
	int offset;  /* how far have we ready into the buffer */
} MSG;

MSG* pgr_m_new();
void pgr_m_init(MSG *m);

void pgr_m_skip(MSG *m, size_t n);
void pgr_m_discard(MSG *m, int fd);
void pgr_m_flush(MSG *m);

int pgr_m_write(MSG *m, const void *buf, size_t len);

int pgr_m_sendn(MSG *m, int fd, size_t len);
int pgr_m_resend(MSG *m, int fd);

int pgr_m_next(MSG *m, int fd);
int pgr_m_relay(MSG *m, int from, int to);

int pgr_m_ignore(MSG *m, int fd, const char *until);

int pgr_m_iserror(MSG *m, const char *code);
void pgr_m_errorf(MSG *m, char *sev, char *code, char *msg, ...);

#define pgr_m_offset(m) ((m)->offset)
#define pgr_m_free(m)   ((m)->free)
#define pgr_m_used(m)   (MSG_BUFSIZ - (m)->free)
#define pgr_m_unread(m) (pgr_m_used(m) - pgr_m_offset(m))
#define pgr_m_reset(m)  ((m)->offset = 0, (m)->free = MSG_BUFSIZ)
#define pgr_m_rewind(m) ((m)->offset = 0)
#define pgr_m_send(m,f) pgr_m_sendn((m), (f), pgr_m_unread(m))

#define pgr_m_buffer(m)   ((m)->buf+(m)->offset)
#define pgr_m_str_at(m,i) ((char*)(m)->buf+(m)->offset+(i))
#define pgr_m_u8_at(m,i)  ((uint8_t)((m)->buf[(m)->offset+(i)]))
#define pgr_m_u16_at(m,i) ((uint16_t)(  (pgr_m_u8_at((m),(i))   <<  8) \
                                      | (pgr_m_u8_at((m),(i)+1))))
#define pgr_m_u32_at(m,i) ((uint32_t)(  (pgr_m_u8_at((m),(i))   << 24) \
                                      | (pgr_m_u8_at((m),(i)+1) << 16) \
                                      | (pgr_m_u8_at((m),(i)+2) <<  8) \
                                      | (pgr_m_u8_at((m),(i)+3))))

/* process control subroutines */
void pgr_abort(int code);

/* randomness subroutines */
int pgr_rand(int start, int end);
void pgr_srand(int seed);

/* configuration subroutines */
int pgr_configure(CONTEXT *c, const char *file, int reload);
void pgr_deconfigure(CONTEXT *c);
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
void pgr_msgf(FILE *io, const char *fmt, ...);
void pgr_logf(FILE *io, int level, const char *fmt, ...);
void pgr_vlogf(FILE *io, int level, const char *fmt, va_list ap);
void pgr_dlogf(FILE *io, int level, const char *file, int line, const char *fn, const char *fmt, ...);
void pgr_vdlogf(FILE *io, int level, const char *file, int line, const char *fn, const char *fmt, va_list ap);
void pgr_hexdump_irl(const void *buf, size_t len);
#ifdef NDEBUG
#  define pgr_debugf(...)
#  define pgr_hexdump(b,l)
#else
#  define pgr_debugf(...) pgr_dlogf(stderr, LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#  define pgr_hexdump(b,l) pgr_hexdump_irl((b), (l))
#endif

/* network subroutines */
int pgr_listen4(const char *ep, int backlog);
int pgr_listen6(const char *ep, int backlog);
int pgr_connect(const char *host, int port, int timeout_ms);
int pgr_sendn(int fd, const void *buf, size_t n);
int pgr_sendf(int fd, const char *fmt, ...);
int pgr_recvn(int fd, void *buf, size_t n);

/* connection subroutines */
void pgr_conn_init(CONTEXT *c, CONNECTION *dst);
void pgr_conn_frontend(CONNECTION *dst, int fd);
void pgr_conn_backend(CONNECTION *dst, BACKEND *b, int i);
int pgr_conn_copy(CONNECTION *dst, CONNECTION *src);
int pgr_conn_connect(CONNECTION *c);
int pgr_conn_accept(CONNECTION *c);

/* thread subroutines */
int pgr_watcher(CONTEXT *c, pthread_t* tid);
int pgr_monitor(CONTEXT *c, pthread_t* tid);
int pgr_worker(CONTEXT *c, pthread_t *tid);

#endif
