/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "host.h"
#include "internal.h"
#include "sandbox.h"

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>

/* Unknown architectures are unavailable, never a partially checked ABI. */
#if defined(__x86_64__)
#define FSIMG_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define FSIMG_AUDIT_ARCH AUDIT_ARCH_AARCH64
#endif

#if defined(FSIMG_AUDIT_ARCH) && defined(LANDLOCK_ACCESS_FS_TRUNCATE)
static int install_filter(void);
static int allow_directory(int, int, uint64_t);

/* The common libc surface plus the bounded host operations used by workers.
 * All unlisted calls fail closed. In particular there is no stat-by-path,
 * metadata mutation, exec, socket, clone, ioctl, dup, or io_uring operation.
 * fstatat is deliberately absent: libc must use the descriptor-only fstat.
 */
#define ALLOW_CALL(name) \
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##name, 0, 1), \
	BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

static int
install_filter(void)
{
	struct sock_filter rules[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
		    offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, FSIMG_AUDIT_ARCH, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
		    offsetof(struct seccomp_data, nr)),
#ifdef __x86_64__
		/* Reject x32 even though its audit architecture is x86_64. */
		BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x40000000u, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
#endif
		ALLOW_CALL(read),
		ALLOW_CALL(write),
		ALLOW_CALL(pread64),
		ALLOW_CALL(pwrite64),
		ALLOW_CALL(close),
		ALLOW_CALL(fstat),
		ALLOW_CALL(lseek),
		ALLOW_CALL(fsync),
		ALLOW_CALL(ftruncate),
		ALLOW_CALL(mkdirat),
		ALLOW_CALL(unlinkat),
		ALLOW_CALL(getdents64),
		ALLOW_CALL(brk),
		ALLOW_CALL(mmap),
		ALLOW_CALL(munmap),
		ALLOW_CALL(mremap),
		ALLOW_CALL(mprotect),
		ALLOW_CALL(madvise),
		ALLOW_CALL(futex),
		ALLOW_CALL(clock_gettime),
		ALLOW_CALL(rt_sigaction),
		ALLOW_CALL(rt_sigprocmask),
		ALLOW_CALL(rt_sigreturn),
		ALLOW_CALL(sigaltstack),
		ALLOW_CALL(exit),
		ALLOW_CALL(exit_group),
		/* Only inspect flags. Duplication and changing flags are denied. */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 4),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
		    offsetof(struct seccomp_data, args[1])),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_GETFL, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
		/* openat is mediated by Landlock; O_PATH could bypass its checks. */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat, 0, 4),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
		    offsetof(struct seccomp_data, args[2])),
		BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, O_PATH, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM)
	};
	struct sock_fprog program;

	program.len = (unsigned short)(sizeof(rules) / sizeof(rules[0]));
	program.filter = rules;
	return (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program));
}
#undef ALLOW_CALL

static int
allow_directory(int ruleset, int fd, uint64_t rights)
{
	struct landlock_path_beneath_attr rule = {
		.allowed_access = rights,
		.parent_fd = fd
	};

	return ((int)syscall(__NR_landlock_add_rule, ruleset,
	    LANDLOCK_RULE_PATH_BENEATH, &rule, 0));
}
#endif
#elif defined(__FreeBSD__)
#include <sys/capsicum.h>
#endif

fsimg_status_t
fsimg_sandbox_install(int input_fd, int work_fd, int source_fd,
    fsimg_error_t *error)
{
	struct stat st;
	int flags;
#if defined(__linux__) && defined(FSIMG_AUDIT_ARCH) && \
    defined(LANDLOCK_ACCESS_FS_TRUNCATE)
	struct landlock_ruleset_attr rules = {
		.handled_access_fs = (LANDLOCK_ACCESS_FS_TRUNCATE << 1) - 1
	};
	uint64_t work_rights, source_rights;
	int abi, ruleset, saved;
#elif defined(__FreeBSD__)
	cap_rights_t rights;
	unsigned mode;
#endif

	fsimg_error_clear(error);
	flags = fcntl(input_fd, F_GETFL);
	if (flags == -1 || fstat(input_fd, &st) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_CONTEXT, "sandbox input validation failed"));
	if ((flags & O_ACCMODE) != O_RDONLY || !S_ISREG(st.st_mode))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "sandbox input requires read-only regular file"));
	if (fstat(work_fd, &st) == -1 || !S_ISDIR(st.st_mode))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "sandbox work root requires directory"));
	if (source_fd != -1) {
		flags = fcntl(source_fd, F_GETFL);
		if (flags == -1 || (flags & O_ACCMODE) != O_RDONLY ||
		    fstat(source_fd, &st) == -1 || !S_ISDIR(st.st_mode))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "sandbox source requires read-only directory"));
	}
#if defined(__linux__) && defined(FSIMG_AUDIT_ARCH) && \
    defined(LANDLOCK_ACCESS_FS_TRUNCATE)
	abi = (int)syscall(__NR_landlock_create_ruleset, NULL, 0,
	    LANDLOCK_CREATE_RULESET_VERSION);
	if (abi == -1 && (errno == ENOSYS || errno == EOPNOTSUPP))
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, errno,
		    FSIMG_OP_CONTEXT, "Landlock unavailable"));
	if (abi == -1)
		goto linux_fail;
	if (abi < 3)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_CONTEXT, "Landlock ABI 3 required"));
	ruleset = (int)syscall(__NR_landlock_create_ruleset, &rules,
	    sizeof(rules), 0);
	if (ruleset == -1)
		goto linux_fail;
	source_rights = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
	work_rights = source_rights | LANDLOCK_ACCESS_FS_WRITE_FILE |
	    LANDLOCK_ACCESS_FS_TRUNCATE | LANDLOCK_ACCESS_FS_MAKE_REG |
	    LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
	    LANDLOCK_ACCESS_FS_REMOVE_DIR;
	if (allow_directory(ruleset, work_fd, work_rights) == -1)
		goto ruleset_fail;
	if (source_fd != -1 &&
	    allow_directory(ruleset, source_fd, source_rights) == -1)
		goto ruleset_fail;
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1)
		goto ruleset_fail;
	if (syscall(__NR_landlock_restrict_self, ruleset, 0) == -1)
		goto ruleset_fail;
	close(ruleset);
	if (install_filter() == -1)
		goto linux_fail;
	return (FSIMG_OK);
ruleset_fail:
	saved = errno;
	close(ruleset);
	errno = saved;
linux_fail:
	return (fsimg_error_set(error, FSIMG_EIO, errno,
	    FSIMG_OP_CONTEXT, "sandbox enforcement failed"));
#elif defined(__FreeBSD__)
	if (cap_getmode(&mode) == -1) {
		if (errno == ENOSYS)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, errno,
			    FSIMG_OP_CONTEXT, "Capsicum unavailable"));
		goto freebsd_fail;
	}
	cap_rights_init(&rights, CAP_READ, CAP_SEEK, CAP_FSTAT, CAP_FCNTL);
	if (cap_rights_limit(input_fd, &rights) == -1)
		goto freebsd_fail;
	if (cap_ioctls_limit(input_fd, NULL, 0) == -1)
		goto freebsd_fail;
	if (cap_fcntls_limit(input_fd, CAP_FCNTL_GETFL) == -1)
		goto freebsd_fail;
	cap_rights_init(&rights, CAP_READ, CAP_WRITE, CAP_LOOKUP, CAP_CREATE,
	    CAP_FSTAT, CAP_SEEK, CAP_FSYNC, CAP_FTRUNCATE, CAP_MKDIRAT,
	    CAP_UNLINKAT, CAP_FCNTL);
	if (cap_rights_limit(work_fd, &rights) == -1)
		goto freebsd_fail;
	if (cap_ioctls_limit(work_fd, NULL, 0) == -1)
		goto freebsd_fail;
	if (cap_fcntls_limit(work_fd, CAP_FCNTL_GETFL) == -1)
		goto freebsd_fail;
	if (source_fd != -1) {
		cap_rights_init(&rights, CAP_READ, CAP_LOOKUP, CAP_FSTAT,
		    CAP_SEEK, CAP_FCNTL);
		if (cap_rights_limit(source_fd, &rights) == -1)
			goto freebsd_fail;
		if (cap_ioctls_limit(source_fd, NULL, 0) == -1)
			goto freebsd_fail;
		if (cap_fcntls_limit(source_fd, CAP_FCNTL_GETFL) == -1)
			goto freebsd_fail;
	}
	if (cap_enter() == -1)
		goto freebsd_fail;
	return (FSIMG_OK);
freebsd_fail:
	return (fsimg_error_set(error, FSIMG_EIO, errno,
	    FSIMG_OP_CONTEXT, "sandbox enforcement failed"));
#else
	return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
	    FSIMG_OP_CONTEXT, "no supported standalone sandbox backend"));
#endif
}
