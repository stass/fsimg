#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# Generate external oracle fixtures; this is not a make check test.
set -eu
exec "${RUBY:-ruby}" "$(dirname "$0")/iso9660-corpus.rb" "$@"
