pgrouter - A Postgres Router
============================

`pgrouter` provides a replication-aware smart router that can sit in front
of a replicated PostgreSQL cluster and intelligently direct queries to the
appropriate node based on type, replication lag and backend availability.

Moving Parts
------------

`pgrouter` is a multi-threaded server that uses a shared dataset for
configuration and backend state.  Threads use mutexes and semaphores to
safely synchronize access to the shared memory.

A **SUPERVISOR** thread handles all UNIX signals, including **SIGHUP**
(reload) and **SIGTERM** (shutdown).

A **WATCHER** thread checks the health of the backend nodes, and update the
shared dataset with availability and replication lag statistics.

A **MONITOR** thread listens for inbound connections on a (loopback)
interface and port.  When a client connects, it dumps monitoring and state
data (in a custom, human-readable format) to the client and closes the
connection.  This can be used for validating the health of a pgrouter
installation.

Multiple **WORKER** threads listen on a TCP port for inbound client
connections and dispatch them to an appropriate, healthy backend.

Some Bits of Code
-----------------

```
/* Status of backends */
#define BACKEND_IS_OK        0  /* backend is up and running    */
#define BACKEND_IS_STARTING  1  /* backend is still connecting  */
#define BACKEND_IS_FAILED    2  /* backend won't connect        */

/* SSL/TLS behaviors */
#define BACKEND_TLS_OFF      0  /* don't do SSL/TLS             */
#define BACKEND_TLS_VERIFY   1  /* do SSL/TLS; verify certs.    */
#define BACKEND_TLS_NOVERIFY 2  /* do SSL/TLS; skip verif.      */

typedef lag_t unsigned long long int;

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
```

Monitoring Data
---------------

When connecting to the monitoring port, clients should recceive something
that looks like this:

```
backends 3/5\n
workers 64\n
clients 32\n
connections 28\n
10.0.0.3:6432 master OK 0\n
10.0.0.1:6432 slave OK 200\n
10.0.0.2:6432 slave OK 208\n
10.0.0.4:6432 slave FAILED\n
10.0.0.5:6432 slave STARTING\n
```

The lines for OK backends contain:

1. host:port
2. type (master or slave)
3. status (OK)
4. lag (0 on master, bytes on slaves)

Usage
-----

Start the daemon using the standard configuration file, and daemonize into
the background:
```
pgrouter
```

Run in the foregound, with a custom configuration file, and lots of logging
(to stderr):

```
pgrouter -Fvvvvv -c /path/to/config
```

Parse the configuration and exit 0 if everything checks out.  Errors should
be reported to stderr and cause an exit 1:

```
pgrouter -t
```

Configuration
-------------

```
# pgrouter.conf

listen *:5432
monitor 127.0.0.1:9881
workers 64
hba /path/to/hba

tls {
  ciphers ALL:!EXP:!LOW
  cert /path/to/pem
  key  /path/to/pem
}

user  vcap
group vcap
pidfile /var/vcap/sys/run/pgrouter/pgrouter.pid
log INFO

health {
  timeout 5s
  check 15s
  database postgres
  username pgtest
  password "sekrit squirrel"
}

backend default {
  tls skipverify;
  lag 200
  weight 100
}
backend 10.0.0.5:6432 { lag 800; }
backend 10.0.0.6:6432

backend default { tls off }
backend 10.0.0.7:6432 { weight 125; lag 500b }
backend 10.0.0.8:6432 { weight 250 }
```
