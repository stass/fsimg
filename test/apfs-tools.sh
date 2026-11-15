#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
set -eu
# The pure parts of the APFS qualification tools, which run on macOS alone:
# their manifests read birth times, which Ruby's File::Stat reports only there.
if test "$(uname -s)" != Darwin; then
    echo 'SKIP: the APFS qualification tools run on macOS only'
    exit 77
fi
if ! command -v ruby >/dev/null 2>&1; then
    echo 'SKIP: Ruby is required for the APFS qualification tool tests'
    exit 77
fi
dir=$(dirname "$0")
ruby "$dir/apfs-qualify-test.rb"
ruby "$dir/apfs-outputs-test.rb"
