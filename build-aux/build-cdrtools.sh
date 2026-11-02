#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
set -eu

if test "$#" != 1; then
    echo 'usage: build-cdrtools.sh new-directory' >&2
    exit 2
fi

cdr_make=${MAKE:-make}
for tool in curl tar sha256sum install "$cdr_make"; do
    if ! command -v "$tool" >/dev/null; then
        echo "build-cdrtools: required development tool is missing: $tool" >&2
        exit 1
    fi
done

# Keep the complete upstream source and notices outside the project. The
# prefix stays private until all three ISO oracles are ready.
umask 077
mkdir "$1"
destination=$(CDPATH= cd -- "$1" && pwd)
archive=$destination/cdrtools-3.02a09.tar.bz2
curl --fail --location --retry 3 --output "$archive" \
    https://downloads.sourceforge.net/project/cdrtools/alpha/cdrtools-3.02a09.tar.bz2
checksum=$(sha256sum < "$archive")
if test "${checksum%% *}" != \
    aa28438f458ef3f314b79f2029db27679dae1d5ffe1569b6de57742511915e81; then
    echo 'build-cdrtools: upstream archive SHA-256 differs' >&2
    exit 1
fi

umask 022
tar -xjf "$archive" -C "$destination"
mv "$destination/cdrtools-3.02" "$destination/source"
umask 077
(
    cd "$destination/source"
    # Upstream's generated links and configuration race during a parallel
    # bootstrap. Do not inherit the caller's fsimg build parallelism.
    unset MAKEFLAGS MFLAGS
    "$cdr_make" -j1 INS_BASE="$destination"
)

mkdir "$destination/bin"
install_oracle() {
    if test "$#" != 1 || test ! -f "$1" || test ! -x "$1"; then
        echo 'build-cdrtools: expected one fresh executable per oracle' >&2
        exit 1
    fi
    install -m 755 "$1" "$destination/bin/"
}

# Copy only the oracles; upstream's install also includes hardware tools.
install_oracle "$destination/source/mkisofs/diag/OBJ/"*/isoinfo
install_oracle "$destination/source/mkisofs/diag/OBJ/"*/isovfy
install_oracle "$destination/source/mkisofs/OBJ/"*/mkisofs
chmod 644 "$archive"
chmod 755 "$destination" "$destination/bin"
echo "cdrtools oracles: $destination/bin; upstream release: 3.02a09"
