/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "socket.h"
#include "test.h"

/* A socket address holds only a short path, so the name is bound relative
 * to the directory while the process works in it, and the working
 * directory is restored; a failure in between exits the test.
 */
int
bind_socket(int directory, const char *name)
{
	struct sockaddr_un address;
	int cwd, fd;

	bzero(&address, sizeof(address));
	address.sun_family = AF_UNIX;
	CHECK(strlen(name) < sizeof(address.sun_path));
	strcpy(address.sun_path, name);
	cwd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(cwd != -1);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(fd != -1);
	CHECK(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
	CHECK(fchdir(directory) == 0);
	CHECK(bind(fd, (const struct sockaddr *)&address, sizeof(address)) == 0);
	CHECK(fchdir(cwd) == 0);
	CHECK(close(cwd) == 0);
	return (fd);
}
