AC_PREREQ(2.63)
AC_INIT([pgrouter],
        [m4_esyscmd([./version.sh])],
        [pgrouter@niftylogic.com],
        ,
        [https://jameshunt.us/oss/pgrouter])

AC_CONFIG_SRCDIR([src/pgrouter.h])
AC_CONFIG_AUX_DIR([build])
AC_CONFIG_MACRO_DIR([build])
AC_CONFIG_HEADERS(config.h)

AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])

AC_PROG_CC

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
