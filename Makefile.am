# Makefile.am

AUTOMAKE_OPTIONS = foreign subdir-objects
ACLOCAL_AMFLAGS = -I build

bin_PROGRAMS = t/authdbtest t/cfgtest t/md5test \
               pgrouter
t_authdbtest_SOURCES = src/authdb.c src/log.c src/abort.c
t_authdbtest_CFLAGS = -DPTEST
t_cfgtest_SOURCES = src/config.c src/log.c src/abort.c src/net.c
t_cfgtest_CFLAGS = -DPTEST
t_md5test_SOURCES = src/md5.c
t_md5test_CFLAGS = -DTEST

pgrouter_SOURCES = src/config.c src/log.c src/init.c src/abort.c src/net.c \
                   src/rand.c src/backend.c src/proto.c \
                   src/watcher.c src/monitor.c src/worker.c \
                   src/main.c
pgrouter_LDADD = -lpthread -lpq
