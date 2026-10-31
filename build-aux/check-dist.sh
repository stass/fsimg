#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
#
# check-dist.sh SRCDIR DISTDIR
#
# Fail when a file git tracks in SRCDIR is missing from the distribution
# directory DISTDIR. Test runners discover their files by listing a
# directory, so a file left out of EXTRA_DIST would silently drop its tests
# from release archives and from the sanitizer builds that run from a
# distdir copy. Two kinds of tracked file stay out on purpose: .gitignore
# files, and the fsspecs/ research drafts, which are working notes rather
# than part of the release. Only files git tracks are compared, so a new
# file counts once it is staged. A release archive carries no git metadata;
# there the check has nothing to compare and passes.
set -eu
LC_ALL=C
export LC_ALL

srcdir=$1
distdir=$2
if ! test -e "$srcdir/.git"; then
    exit 0
fi
# core.quotePath=false keeps non-ASCII names unquoted. Names with quotes,
# backslashes or control characters stay quoted by git; none are tracked.
if ! files=$(git -C "$srcdir" -c core.quotePath=false ls-files); then
    echo "check-dist: git is required to check a git checkout ($srcdir)" >&2
    exit 1
fi
# A pipe, not a here-document: a shell that cannot create a here-document's
# temporary file skips the loop and would report success. The group's exit
# status is the pipeline's, so set -e ends the script with it.
printf '%s\n' "$files" | {
    missing=0
    while IFS= read -r file; do
        case $file in
        .gitignore | */.gitignore | fsspecs/*)
            continue
            ;;
        esac
        if test -e "$distdir/$file"; then
            continue
        fi
        if test "$missing" -eq 0; then
            echo 'check-dist: tracked files missing from the distribution (add them to EXTRA_DIST):' >&2
        fi
        printf '  %s\n' "$file" >&2
        missing=1
    done
    exit "$missing"
}
