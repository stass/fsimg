#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
set -eu
if ! command -v ruby >/dev/null 2>&1; then
    echo 'SKIP: Ruby is required for HFS Plus integration'
    exit 77
fi
ruby "$(dirname "$0")/hfsplus-capture-test.rb"
ruby "$(dirname "$0")/hfsplus-integration-test.rb"
ruby "$(dirname "$0")/hfsplus-qualify-test.rb" ./hfsplus-write-gen ./fsimg
exec ruby "$(dirname "$0")/hfsplus-integration.rb" ./hfsplus-write-gen ./fsimg
