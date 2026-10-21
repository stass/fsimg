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
FSIMG_DEV_TOOL([MDIR], [mdir], [mtools directory listing])
FSIMG_DEV_TOOL([MTYPE], [mtype], [mtools file reading])
FSIMG_DEV_TOOL([FATCAT], [fatcat], [pinned fatcat FAT oracle])
FSIMG_DEV_TOOL([PYTHON3], [python3], [Python 3 interpreter for tests])
FSIMG_DEV_TOOL([RUBY], [ruby], [Ruby interpreter for specification tests])
FSIMG_DEV_TOOL([PKG_CONFIG], [pkg-config], [pkg-config for installation checks])
FSIMG_DEV_TOOL([SGDISK], [sgdisk], [gptfdisk GPT partitioner], [$fsimg_sbin_path])
FSIMG_DEV_TOOL([SFDISK], [sfdisk], [util-linux MBR and GPT partitioner],
    [$fsimg_sbin_path:/usr/local/opt/util-linux/sbin:/opt/homebrew/opt/util-linux/sbin])
FSIMG_DEV_TOOL([MFORMAT], [mformat], [mtools FAT formatter])
FSIMG_DEV_TOOL([HDIUTIL], [hdiutil], [macOS disk image tool])
FSIMG_DEV_TOOL([MKFS_EXFAT], [mkfs.exfat], [exfatprogs exFAT formatter], [$fsimg_sbin_path])
FSIMG_DEV_TOOL([FSCK_EXFAT], [fsck.exfat], [exfatprogs exFAT checker], [$fsimg_sbin_path])
FSIMG_DEV_TOOL([QEMU_IMG], [qemu-img], [QEMU disk image tool])
FSIMG_DEV_TOOL([SIMG2IMG], [simg2img], [Android sparse image expander])
FSIMG_DEV_TOOL([IMG2SIMG], [img2simg], [Android sparse image producer])
dnl Not GZIP: gzip(1) reads options from an environment variable of that name.
FSIMG_DEV_TOOL([GZIP_PROGRAM], [gzip], [gzip compressor])
FSIMG_DEV_TOOL([PIGZ], [pigz], [parallel gzip compressor])
AS_IF([test "$ac_cv_func_pread$ac_cv_func_pwrite$ac_cv_func_ftruncate$ac_cv_func_fsync$ac_cv_func_linkat" != yesyesyesyesyes], [AC_MSG_ERROR([required POSIX I/O unavailable])])
# The system zlib is the one third-party library the core links: DMG zlib
# chunks and gzip need it, and inflate parses untrusted images, so the
# host's security fixes must apply. It is never vendored. pkg-config
# supplies its flags where it describes zlib; otherwise the header and the
# library are found on the default paths. ZLIB_LIBS also becomes the
# installed metadata's Libs.private.
AC_ARG_VAR([ZLIB_CFLAGS], [C compiler flags for the system zlib])
AC_ARG_VAR([ZLIB_LIBS], [linker flags for the system zlib])
AS_IF([test -z "$ZLIB_CFLAGS$ZLIB_LIBS" && test -n "$PKG_CONFIG" &&
    "$PKG_CONFIG" --exists zlib],
    [ZLIB_CFLAGS=`"$PKG_CONFIG" --cflags zlib`
     ZLIB_LIBS=`"$PKG_CONFIG" --libs zlib`])
AS_IF([test -z "$ZLIB_LIBS"], [ZLIB_LIBS=-lz])
fsimg_saved_CPPFLAGS=$CPPFLAGS
fsimg_saved_LIBS=$LIBS
CPPFLAGS="$CPPFLAGS $ZLIB_CFLAGS"
LIBS="$ZLIB_LIBS $LIBS"
AC_CHECK_HEADER([zlib.h], [],
    [AC_MSG_ERROR([fsimg requires the system zlib: zlib.h was not found (install the zlib development package, or set ZLIB_CFLAGS)])])
AC_MSG_CHECKING([whether the system zlib links])
AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <zlib.h>]],
    [[z_stream s = { 0 }; return (inflateInit(&s) != Z_OK || zlibVersion()[0] == '\0');]])],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no])
     AC_MSG_ERROR([fsimg requires the system zlib: linking with $ZLIB_LIBS failed (install the zlib development package, or set ZLIB_LIBS)])])
CPPFLAGS=$fsimg_saved_CPPFLAGS
LIBS=$fsimg_saved_LIBS
AC_CONFIG_FILES([Makefile libfsimg.pc test/fat-integration.sh test/exfat-integration.sh
    test/qnx6-integration.sh test/qnx4-integration.sh test/layers-integration.sh
    test/containers-integration.sh test/compression-integration.sh
    test/cli.sh test/baseline.sh test/spec.sh],
    [test ! -f test/fat-integration.sh || chmod +x test/fat-integration.sh
     test ! -f test/exfat-integration.sh || chmod +x test/exfat-integration.sh
     test ! -f test/qnx6-integration.sh || chmod +x test/qnx6-integration.sh
     test ! -f test/qnx4-integration.sh || chmod +x test/qnx4-integration.sh
     test ! -f test/layers-integration.sh || chmod +x test/layers-integration.sh
     test ! -f test/containers-integration.sh || chmod +x test/containers-integration.sh
     test ! -f test/compression-integration.sh || chmod +x test/compression-integration.sh
     test ! -f test/cli.sh || chmod +x test/cli.sh
     test ! -f test/baseline.sh || chmod +x test/baseline.sh
     test ! -f test/spec.sh || chmod +x test/spec.sh])
AC_OUTPUT
