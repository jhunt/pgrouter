# Makefile.am

AUTOMAKE_OPTIONS = foreign subdir-objects
ACLOCAL_AMFLAGS = -I build

bin_PROGRAMS = t/authdbtest t/authtest t/cfgtest t/md5test \
               t/driver \
               pgrouter
t_authdbtest_SOURCES = src/authdb.c src/log.c src/abort.c
t_authdbtest_CFLAGS = -DPTEST
t_authdbtest_LDADD = -lpthread
t_authtest_SOURCES = src/authdb.c src/log.c src/abort.c src/md5.c src/init.c
t_authtest_CFLAGS = -DATEST
t_authtest_LDADD = -lpthread
t_cfgtest_SOURCES = src/config.c src/log.c src/abort.c src/net.c
t_cfgtest_CFLAGS = -DPTEST
t_md5test_SOURCES = src/md5.c
t_md5test_CFLAGS = -DTEST

t_driver_SOURCES = driver/main.c
t_driver_LDADD = -lpq

pgrouter_SOURCES = src/config.c src/log.c src/init.c src/abort.c src/net.c \
                   src/rand.c src/msg.c src/md5.c src/authdb.c src/conn.c \
                   src/watcher.c src/monitor.c src/worker.c \
                   src/main.c
pgrouter_LDADD = -lpthread -lpq
