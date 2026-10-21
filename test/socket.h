/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_TEST_SOCKET_H
#define FSIMG_TEST_SOCKET_H

/* Bind a Unix-domain socket at name beneath directory and return its
 * descriptor, which the caller closes; name remains until the caller or
 * its fixture removes it. Any failure ends the test.
 */
int bind_socket(int, const char *);

#endif
