# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
AC_INIT([fsimg], [0.1.0])
AC_CONFIG_SRCDIR([lib/core/context.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall dist-xz no-dist-gzip])
AC_CONFIG_HEADERS([config.h])
AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE
AC_PROG_CC
AC_PROG_CXX
AM_PROG_AR
LT_INIT
AC_CHECK_SIZEOF([off_t], [], [[#include <sys/types.h>]])
AS_IF([test "$ac_cv_sizeof_off_t" -lt 8], [AC_MSG_ERROR([64-bit off_t required])])
AC_CHECK_SIZEOF([void *])
AS_IF([test "$ac_cv_sizeof_void_p" -lt 8], [AC_MSG_ERROR([64-bit host required])])
AC_CHECK_FUNCS([pread pwrite ftruncate fsync linkat])
AS_IF([test "$ac_cv_func_pread$ac_cv_func_pwrite$ac_cv_func_ftruncate$ac_cv_func_fsync$ac_cv_func_linkat" != yesyesyesyesyes], [AC_MSG_ERROR([required POSIX I/O unavailable])])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
