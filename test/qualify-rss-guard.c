/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#if defined(__FreeBSD__)
#include <sys/mman.h>
#endif
#include <sys/resource.h>
#include <sys/wait.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "host.h"
#include "test.h"
#include "worker.h"

int fsimg_rss_qualification_main(int, char **);
int fsimg_measured_main(int, char **);

/* A successful stand-in isolates qualification-entry validation from CLI
 * behavior. The initial-map guard must reject before this body can matter.
 */
int
fsimg_measured_main(int argc FSIMG_UNUSED, char **argv FSIMG_UNUSED)
{
	return (0);
}

int
fsimg_host_executable(char *buffer FSIMG_UNUSED, size_t size FSIMG_UNUSED)
{
	errno = ENOSYS;
	return (-1);
}

int
main(void)
{
#if defined(__FreeBSD__)
	struct rlimit address, stack;
	volatile unsigned char *mapping;
	char *arguments[] = { "rss-guard", "--rss-vm-bound", "16777216", "formats", NULL };
	char diagnostic[4096];
	void *warm;
	pid_t child;
	size_t length;
	ssize_t count;
	int channel[2], status;

	CHECK(geteuid() != 0 && pipe(channel) == 0);
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		CHECK(close(channel[0]) == 0 && dup2(channel[1], STDERR_FILENO) == STDERR_FILENO);
		CHECK(close(channel[1]) == 0);
		/* Keep a small allocation reusable after the cap is lowered. This
		 * prevents a later unrelated allocator refusal from hiding the bug.
		 */
		warm = calloc(5, sizeof(char *));
		CHECK(warm != NULL);
		free(warm);
		mapping = mmap(NULL, (size_t)128 << 20, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANON, -1, 0);
		CHECK(mapping != MAP_FAILED);
		for (size_t i = 0; i < (size_t)128 << 20; i += 4096)
			mapping[i] = 1;
		stack.rlim_cur = stack.rlim_max = (rlim_t)8 << 20;
		address.rlim_cur = address.rlim_max = ((rlim_t)80 << 20) / FSIMG_CLI_PROCESS_BOUND;
		CHECK(setrlimit(RLIMIT_STACK, &stack) == 0 && setrlimit(RLIMIT_AS, &address) == 0);
		_exit(fsimg_rss_qualification_main(4, arguments));
	}
	CHECK(close(channel[1]) == 0);
	CHECK(waitpid(child, &status, 0) == child);
	length = 0;
	for (;;) {
		CHECK(length < sizeof(diagnostic) - 1);
		count = read(channel[0], diagnostic + length, sizeof(diagnostic) - 1 - length);
		if (count == -1 && errno == EINTR)
			continue;
		CHECK(count >= 0);
		if (count == 0)
			break;
		length += (size_t)count;
	}
	diagnostic[length] = '\0';
	CHECK(close(channel[0]) == 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 1 ||
	    strstr(diagnostic, "process.ki_size") == NULL)
		fprintf(stderr, "unexpected guard result status=%d: %s\n", status, diagnostic);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
	CHECK(strstr(diagnostic, "process.ki_size") != NULL);
	CHECK(printf("oversized initial address space is rejected despite matching limits\n") >= 0);
	CHECK(fflush(stdout) == 0);
	return (0);
#else
	puts("SKIP: initial virtual-memory guard applies to FreeBSD qualification");
	return (77);
#endif
}
