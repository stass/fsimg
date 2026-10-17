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
AC_CHECK_FUNCS([posix_spawn_file_actions_addclosefrom_np])
AC_PATH_PROG([MKFS_FAT], [mkfs.fat], [], [$PATH:/usr/sbin:/sbin:/usr/local/sbin:/opt/homebrew/sbin])
AC_PATH_PROG([FSCK_FAT], [fsck.fat], [], [$PATH:/usr/sbin:/sbin:/usr/local/sbin:/opt/homebrew/sbin])
AC_PATH_PROG([NEWFS_MSDOS], [newfs_msdos], [], [$PATH:/sbin])
AC_PATH_PROG([FSCK_MSDOSFS], [fsck_msdosfs], [], [$PATH:/sbin])
AC_PATH_PROG([MCOPY], [mcopy])
AC_PATH_PROG([MMD], [mmd])
AC_PATH_PROG([MDEL], [mdel])
AC_PATH_PROG([FATCAT], [fatcat])
AC_PATH_PROG([MKFS_EXFAT], [mkfs.exfat], [], [$PATH:/usr/sbin:/sbin:/usr/local/sbin:/opt/homebrew/sbin])
AC_PATH_PROG([FSCK_EXFAT], [fsck.exfat], [], [$PATH:/usr/sbin:/sbin:/usr/local/sbin:/opt/homebrew/sbin])
AS_IF([test "$ac_cv_func_pread$ac_cv_func_pwrite$ac_cv_func_ftruncate$ac_cv_func_fsync$ac_cv_func_linkat" != yesyesyesyesyes], [AC_MSG_ERROR([required POSIX I/O unavailable])])
AC_CONFIG_FILES([Makefile test/fat-integration.sh test/exfat-integration.sh],
    [test ! -f test/fat-integration.sh || chmod +x test/fat-integration.sh
     test ! -f test/exfat-integration.sh || chmod +x test/exfat-integration.sh])
AC_OUTPUT
