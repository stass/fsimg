/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
/*
 * compat/private.c with its mkdirat routed through test/private.c's hook,
 * which stands in for a writer of the parent between creation and the
 * reopen.
 */
#define mkdirat fsimg_test_mkdirat
#include "../compat/private.c"
