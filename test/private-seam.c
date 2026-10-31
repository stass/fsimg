/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
/*
 * compat/private.c with two of its host calls routed through
 * test/private.c's hooks, which stand in for another user: mkdirat, after
 * which a writer of the parent can replace the new directory, and the
 * calls of the platform's ACL query, before which a user an ACL lets
 * change the directory's security can still change its mode.
 */
#define mkdirat fsimg_test_mkdirat
#if defined(__APPLE__)
#define acl_get_fd_np fsimg_test_acl_get_fd_np
#elif defined(__FreeBSD__)
#define acl_get_fd_np fsimg_test_acl_get_fd_np
#define fpathconf fsimg_test_fpathconf
#elif defined(__linux__)
#define fgetxattr fsimg_test_fgetxattr
#endif
#include "../compat/private.c"
