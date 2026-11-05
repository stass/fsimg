#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
set -eu
if ! command -v ruby >/dev/null 2>&1; then
    echo 'SKIP: Ruby is required for HFS Plus integration'
    exit 77
fi
exec ruby "$(dirname "$0")/hfsplus-integration.rb" ./hfsplus-write-gen
