# Makefile.am

AUTOMAKE_OPTIONS = foreign subdir-objects
ACLOCAL_AMFLAGS = -I build

bin_PROGRAMS = t/cfgtest \
               pgrouter
t_cfgtest_SOURCES = src/config.c src/log.c src/abort.c src/net.c
t_cfgtest_CFLAGS = -DPTEST

pgrouter_SOURCES = src/config.c src/log.c src/init.c src/abort.c src/net.c \
                   src/rand.c src/backend.c src/proto.c \
                   src/watcher.c src/monitor.c src/worker.c \
                   src/main.c
pgrouter_LDADD = -lpthread -lpq
