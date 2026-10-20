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
AC_CHECK_DECLS([POSIX_SPAWN_CLOEXEC_DEFAULT], [], [], [[#include <spawn.h>]])
AS_IF([test "$ac_cv_func_posix_spawn_file_actions_addclosefrom_np" != yes &&
    test "$ac_cv_have_decl_POSIX_SPAWN_CLOEXEC_DEFAULT" != yes],
    [AC_MSG_ERROR([fsimg CLI requires complete spawn-time descriptor closure: POSIX_SPAWN_CLOEXEC_DEFAULT or posix_spawn_file_actions_addclosefrom_np])])
# FSIMG_DEV_TOOL(VARIABLE, PROGRAM, DESCRIPTION, [SEARCH-PATH])
# Find a development tool used by tests. VARIABLE is precious, so a path
# given to configure in the environment or as an argument is recorded in
# config.status --config, which sanitize.sh replays into sanitizer builds.
m4_define([FSIMG_DEV_TOOL], [AC_ARG_VAR([$1], [$3])
AC_PATH_PROG([$1], [$2], [], [$4])])
fsimg_sbin_path=$PATH:/usr/sbin:/sbin:/usr/local/sbin:/opt/homebrew/sbin
FSIMG_DEV_TOOL([MKFS_FAT], [mkfs.fat], [dosfstools FAT formatter], [$fsimg_sbin_path])
FSIMG_DEV_TOOL([FSCK_FAT], [fsck.fat], [dosfstools FAT checker], [$fsimg_sbin_path])
FSIMG_DEV_TOOL([NEWFS_MSDOS], [newfs_msdos], [BSD FAT formatter], [$PATH:/sbin])
FSIMG_DEV_TOOL([FSCK_MSDOSFS], [fsck_msdosfs], [BSD FAT checker], [$PATH:/sbin])
FSIMG_DEV_TOOL([MCOPY], [mcopy], [mtools file copy])
FSIMG_DEV_TOOL([MMD], [mmd], [mtools directory creation])
FSIMG_DEV_TOOL([MDEL], [mdel], [mtools file deletion])
FSIMG_DEV_TOOL([FATCAT], [fatcat], [pinned fatcat FAT oracle])
FSIMG_DEV_TOOL([PYTHON3], [python3], [Python 3 interpreter for tests])
FSIMG_DEV_TOOL([PKG_CONFIG], [pkg-config], [pkg-config for installation checks])
FSIMG_DEV_TOOL([MKFS_EXFAT], [mkfs.exfat], [exfatprogs exFAT formatter], [$fsimg_sbin_path])
FSIMG_DEV_TOOL([FSCK_EXFAT], [fsck.exfat], [exfatprogs exFAT checker], [$fsimg_sbin_path])
AS_IF([test "$ac_cv_func_pread$ac_cv_func_pwrite$ac_cv_func_ftruncate$ac_cv_func_fsync$ac_cv_func_linkat" != yesyesyesyesyes], [AC_MSG_ERROR([required POSIX I/O unavailable])])
AC_CONFIG_FILES([Makefile libfsimg.pc test/fat-integration.sh test/exfat-integration.sh
    test/qnx6-integration.sh test/cli.sh test/baseline.sh],
    [test ! -f test/fat-integration.sh || chmod +x test/fat-integration.sh
     test ! -f test/exfat-integration.sh || chmod +x test/exfat-integration.sh
     test ! -f test/qnx6-integration.sh || chmod +x test/qnx6-integration.sh
     test ! -f test/cli.sh || chmod +x test/cli.sh
     test ! -f test/baseline.sh || chmod +x test/baseline.sh])
AC_OUTPUT
