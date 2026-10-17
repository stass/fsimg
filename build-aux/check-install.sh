#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
set -eu

if test "$#" != 10; then
    echo 'usage: check-install.sh build make cc cxx cppflags cflags cxxflags ldflags pkg-config python3' >&2
    exit 2
fi
qualification_origin_build=$1
qualification_make=$2
qualification_cc=$3
qualification_cxx=$4
qualification_cppflags=$5
qualification_cflags=$6
qualification_cxxflags=$7
qualification_ldflags=$8
qualification_pkg_config=$9
shift 9
qualification_python=$1

if test -z "$qualification_pkg_config" || test -z "$qualification_python"; then
    echo 'SKIP: installation qualification requires pkg-config and Python 3 development tools'
    exit 77
fi
qualification_root=$(mktemp -d "${TMPDIR:-/tmp}/fsimg-install.XXXXXXXX")
trap 'qualification_status=$?; if test "$qualification_status" = 0; then rm -rf "$qualification_root"; else echo "installation fixtures retained: $qualification_root" >&2; fi' 0
qualification_build=$qualification_root/build
qualification_source=$qualification_root/source
qualification_prefix=$qualification_root/prefix
mkdir -m 700 "$qualification_build" "$qualification_root/tmp" \
    "$qualification_root/output" "$qualification_root/scratch"

# Own the installation layout by configuring a fresh build. Inherited make
# overrides, CONFIG_SITE and install-directory variables cannot redirect its
# writes. Compiler choices/flags are preserved, not re-created at call sites.
run_clean() {
    env -i PATH="$PATH" LC_ALL=C TZ=UTC TMPDIR="$qualification_root/tmp" \
        CONFIG_SITE=/dev/null CC="$qualification_cc" CXX="$qualification_cxx" \
        CPPFLAGS="$qualification_cppflags" CFLAGS="$qualification_cflags" \
        CXXFLAGS="$qualification_cxxflags" LDFLAGS="$qualification_ldflags" \
        PKG_CONFIG="$qualification_pkg_config" PYTHON3="$qualification_python" "$@"
}
# Distribution owns the source manifest and includes generated build support.
# Its clean copy has no config.status even when the caller builds in-tree.
run_clean "$qualification_make" -C "$qualification_origin_build" distdir \
    "distdir=$qualification_source" > "$qualification_root/distribution.log" 2>&1
mkdir "$qualification_root/no-closure"
cd "$qualification_root/no-closure"
if run_clean env ac_cv_func_posix_spawn_file_actions_addclosefrom_np=no \
    ac_cv_have_decl_POSIX_SPAWN_CLOEXEC_DEFAULT=no \
    "$qualification_source/configure" --prefix="$qualification_prefix" \
    > "$qualification_root/no-closure.log" 2>&1; then
    echo 'FAIL: configure accepted a host without complete spawn-time descriptor closure' >&2
    exit 1
fi
if ! grep -q 'requires complete spawn-time descriptor closure' "$qualification_root/no-closure.log"; then
    cat "$qualification_root/no-closure.log" >&2
    exit 1
fi
cd "$qualification_build"
run_clean "$qualification_source/configure" --prefix="$qualification_prefix" \
    --enable-shared --enable-static > "$qualification_root/configure.log" 2>&1
run_clean "$qualification_make" -j2 > "$qualification_root/build.log" 2>&1
run_clean "$qualification_make" install > "$qualification_root/install.log" 2>&1

# Metadata is parsed with argument boundaries and checked against this prefix;
# an inherited PKG_CONFIG_PATH cannot substitute another package's records.
run_clean "$qualification_python" "$qualification_source/build-aux/check-installed.py" \
    "$qualification_pkg_config" "$qualification_prefix"
run_clean "$qualification_python" "$qualification_source/build-aux/check-installed.py" \
    --self-test "$qualification_pkg_config"
run_clean "$qualification_make" check-installed-consumers \
    > "$qualification_root/consumers.log" 2>&1
"$qualification_build/provider-static" "$qualification_root/output" static.img \
    "$qualification_root/scratch"
LD_LIBRARY_PATH="$qualification_prefix/lib" DYLD_LIBRARY_PATH="$qualification_prefix/lib" \
    "$qualification_build/provider-shared" "$qualification_root/output" shared.img \
    "$qualification_root/scratch"
"$qualification_prefix/bin/fsimg" verify -j -d \
    -i "$qualification_root/output/shared.img" > "$qualification_root/verify.log"

# The command carries its core directly; a moved copy still starts its worker
# with no private-library search environment or installed helper executable.
mkdir -m 700 "$qualification_root/relocated"
cp "$qualification_prefix/bin/fsimg" "$qualification_root/relocated/fsimg"
"$qualification_root/relocated/fsimg" verify -j -d \
    -i "$qualification_root/output/static.img" > "$qualification_root/relocated.log"

case "$(uname -s)" in
    Darwin)
        otool -L "$qualification_prefix/bin/fsimg" > "$qualification_root/dependencies.log"
        awk '
            NR > 1 && $1 != "/usr/lib/libSystem.B.dylib" { bad=1 }
            END { exit bad }
        ' "$qualification_root/dependencies.log"
        ;;
    FreeBSD)
        ldd "$qualification_prefix/bin/fsimg" > "$qualification_root/dependencies.log"
        awk '
            NR == 1 { next }
            $1 ~ /^libc\.so\.[0-9]+$/ { next }
            $1 ~ /^libsys\.so\.[0-9]+$/ { next }
            $1 == "[vdso]" { next }
            { bad=1 }
            END { exit bad }
        ' "$qualification_root/dependencies.log"
        ;;
    Linux)
        ldd "$qualification_prefix/bin/fsimg" > "$qualification_root/dependencies.log"
        awk '
            $1 ~ /^linux-vdso\.so\./ { next }
            $1 == "libc.so.6" { next }
            $1 ~ /^\/.*\/ld-linux[^/]*\.so\.[0-9]+$/ { next }
            { bad=1 }
            END { exit bad }
        ' "$qualification_root/dependencies.log"
        ;;
esac
cat "$qualification_root/dependencies.log"
run_clean "$qualification_make" uninstall > "$qualification_root/uninstall.log" 2>&1
qualification_leftovers=$(find "$qualification_prefix" \( -type f -o -type l \) -print)
if test -n "$qualification_leftovers"; then
    echo "uninstall retained files: $qualification_leftovers" >&2
    exit 1
fi
echo 'private-prefix install/uninstall, checked metadata, static/shared consumers and relocated CLI pass'
