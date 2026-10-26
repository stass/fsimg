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
#include <strings.h>
#include <unistd.h>

#include "host.h"
#include "internal.h"
#include "sandbox.h"

_Static_assert(FSIMG_SANDBOX_SETTINGS == 2,
    "the Capsicum read-only roles list both file settings");

static fsimg_status_t check_input(int, bool, fsimg_error_t *);
static fsimg_status_t check_roles(const fsimg_sandbox_caps_t *, fsimg_error_t *);

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
 * The filter cannot see a path argument, so newfstatat and statx stay denied
 * even for an empty path; fsimg_host_fstat issues fstat instead.
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
		/* Every other call, statx and newfstatat among them, is denied. */
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

static int restrict_descriptor(int, cap_rights_t *, unsigned);

static int
restrict_descriptor(int fd, cap_rights_t *rights, unsigned fcntls)
{
	if (cap_rights_limit(fd, rights) == -1)
		return (-1);
	if (cap_ioctls_limit(fd, NULL, 0) == -1)
		return (-1);
	if (cap_fcntls_limit(fd, fcntls) == -1)
		return (-1);
	return (0);
}
#endif

void
fsimg_sandbox_caps_init(fsimg_sandbox_caps_t *caps)
{
	bzero(caps, sizeof(*caps));
	caps->input = caps->staging = caps->destination = caps->scratch = -1;
	caps->roots = caps->raw = caps->spool = caps->manifest = -1;
	caps->job = caps->control = caps->data = caps->cancel = -1;
	for (unsigned i = 0; i < FSIMG_SANDBOX_SETTINGS; i++)
		caps->settings[i] = -1;
}

bool
fsimg_sandbox_caps_names(const fsimg_sandbox_caps_t *caps, int fd)
{
	const int roles[] = { caps->input, caps->staging, caps->destination,
	    caps->scratch, caps->raw, caps->spool, caps->manifest };

	for (unsigned i = 0; i < sizeof(roles) / sizeof(roles[0]); i++) {
		if (roles[i] == fd)
			return (true);
	}
	for (unsigned i = 0; i < FSIMG_SANDBOX_SETTINGS; i++) {
		if (caps->settings[i] == fd)
			return (true);
	}
	return (false);
}

fsimg_status_t
fsimg_sandbox_install(int input_fd, int destination_fd, int source_fd,
    fsimg_error_t *error)
{
	fsimg_sandbox_caps_t caps;
	fsimg_input_t input = { 0 };
	fsimg_status_t status;

	fsimg_sandbox_caps_init(&caps);
	caps.input = input_fd;
	if (input_fd != -1) {
		status = check_input(input_fd, false, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_input_prepare(input_fd, NULL, NULL, &input, error);
		if (status != FSIMG_OK)
			return (status);
		caps.image_input = &input;
	}
	caps.destination = destination_fd;
	caps.roots = source_fd;
	caps.root_count = source_fd == -1 ? 0 : 1;
	return (fsimg_sandbox_install_caps(&caps, error));
}

const char *
fsimg_sandbox_backend(void)
{
#if defined(__linux__) && defined(FSIMG_AUDIT_ARCH) && \
    defined(LANDLOCK_ACCESS_FS_TRUNCATE)
	return ("landlock+seccomp");
#elif defined(__FreeBSD__)
	return ("capsicum");
#else
	return ("none");
#endif
}

/* The input pipe's read end, from pipe(2): read-only, except on FreeBSD,
 * whose pipes are bidirectional, so both ends are O_RDWR. There, when
 * confined, Capsicum limits the input to reading; elsewhere nothing would
 * stop the worker writing an O_RDWR FIFO, so only a read-only one is
 * accepted.
 */
static bool
readable_pipe(int flags, const struct stat *st)
{
	int mode = flags & O_ACCMODE;

	if (!S_ISFIFO(st->st_mode))
		return (false);
#ifdef __FreeBSD__
	return (mode == O_RDONLY || mode == O_RDWR);
#else
	return (mode == O_RDONLY);
#endif
}

/* A read-only input: a regular file, or the read end of a pipe when pipe
 * says so.
 */
static fsimg_status_t
check_input(int fd, bool pipe, fsimg_error_t *error)
{
	struct stat st;
	int flags;

	flags = fcntl(fd, F_GETFL);
	if (flags == -1 || fsimg_host_fstat(fd, &st) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_CONTEXT, "sandbox input validation failed"));
	if (pipe && !readable_pipe(flags, &st))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "sandbox input requires the read end of a pipe"));
	if (!pipe && ((flags & O_ACCMODE) != O_RDONLY || !S_ISREG(st.st_mode)))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "sandbox input requires read-only regular file"));
	return (FSIMG_OK);
}

/* Every role must be the object its grant assumes before any backend runs:
 * the prepared image input a read-only file/device or relay pipe, and each
 * file setting a read-only regular file; the three writable roles
 * directories; each root a read-only directory
 * or regular file; the raw intermediate and the spool read/write regular
 * files no name reaches; and the manifest a write-only regular file with
 * its one staged name. The library checks those three files again when it
 * adopts them.
 */
static fsimg_status_t
check_roles(const fsimg_sandbox_caps_t *caps, fsimg_error_t *error)
{
	const int directories[] = { caps->staging, caps->destination, caps->scratch };
	const int handed_over[] = { caps->raw, caps->spool };
	struct stat st;
	int flags, fd;
	fsimg_status_t status;

	if (caps->input != -1) {
		if (caps->image_input == NULL)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "sandbox image description is missing"));
		status = fsimg_input_validate(caps->image_input, error);
		if (status != FSIMG_OK)
			return (status);
		if (caps->image_input->kind == FSIMG_INPUT_STREAM)
			status = check_input(caps->input, true, error);
		else
			status = fsimg_input_check(caps->input, caps->image_input, error);
		if (status != FSIMG_OK)
			return (status);
	} else if (caps->image_input != NULL &&
	    caps->image_input->kind != FSIMG_INPUT_ABSENT) {
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "sandbox image descriptor is missing"));
	}
	for (unsigned i = 0; i < FSIMG_SANDBOX_SETTINGS; i++) {
		if (caps->settings[i] == -1)
			continue;
		status = check_input(caps->settings[i], false, error);
		if (status != FSIMG_OK)
			return (status);
	}
	for (unsigned i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
		if (directories[i] != -1 &&
		    (fsimg_host_fstat(directories[i], &st) == -1 || !S_ISDIR(st.st_mode)))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "sandbox work root requires directory"));
	}
	if ((caps->roots == -1) != (caps->root_count == 0))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "sandbox roots are inconsistent"));
	for (unsigned i = 0; i < caps->root_count; i++) {
		fd = caps->roots + (int)i;
		flags = fcntl(fd, F_GETFL);
		if (flags == -1 || (flags & O_ACCMODE) != O_RDONLY ||
		    fsimg_host_fstat(fd, &st) == -1 || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "sandbox source requires read-only directory or file"));
	}
	for (unsigned i = 0; i < 2; i++) {
		fd = handed_over[i];
		if (fd == -1)
			continue;
		flags = fcntl(fd, F_GETFL);
		if (flags == -1 || (flags & O_ACCMODE) != O_RDWR ||
		    fsimg_host_fstat(fd, &st) == -1 || !S_ISREG(st.st_mode) ||
		    st.st_nlink != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "sandbox handed-over file requires an unlinked read/write file"));
	}
	if (caps->manifest != -1) {
		flags = fcntl(caps->manifest, F_GETFL);
		if (flags == -1 || (flags & O_ACCMODE) != O_WRONLY ||
		    fsimg_host_fstat(caps->manifest, &st) == -1 || !S_ISREG(st.st_mode) ||
		    st.st_nlink != 1)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "sandbox manifest requires a write-only file with one name"));
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_sandbox_install_caps(const fsimg_sandbox_caps_t *caps,
    fsimg_error_t *error)
{
	fsimg_status_t status;
#if defined(__linux__) && defined(FSIMG_AUDIT_ARCH) && \
    defined(LANDLOCK_ACCESS_FS_TRUNCATE)
	struct landlock_ruleset_attr rules = {
		.handled_access_fs = (LANDLOCK_ACCESS_FS_TRUNCATE << 1) - 1
	};
	const struct {
		int		 fd;
		uint64_t	 rights;
	} grants[] = {
		{ caps->staging, LANDLOCK_ACCESS_FS_READ_FILE |
		    LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE |
		    LANDLOCK_ACCESS_FS_MAKE_REG },
		{ caps->destination, LANDLOCK_ACCESS_FS_READ_FILE |
		    LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE |
		    LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_READ_DIR |
		    LANDLOCK_ACCESS_FS_MAKE_DIR },
		{ caps->scratch, LANDLOCK_ACCESS_FS_READ_FILE |
		    LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE |
		    LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_REMOVE_FILE }
	};
	struct stat st;
	int abi, ruleset, saved;
#elif defined(__FreeBSD__)
	cap_rights_t rights;
	unsigned mode;
#endif

	fsimg_error_clear(error);
	status = check_roles(caps, error);
	if (status != FSIMG_OK)
		return (status);
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
	for (unsigned i = 0; i < sizeof(grants) / sizeof(grants[0]); i++) {
		if (grants[i].fd != -1 &&
		    allow_directory(ruleset, grants[i].fd, grants[i].rights) == -1)
			goto ruleset_fail;
	}
	/* A file root is read through its descriptor alone, which Landlock does
	 * not mediate, so only directory roots need a rule.
	 */
	for (unsigned i = 0; i < caps->root_count; i++) {
		if (fsimg_host_fstat(caps->roots + (int)i, &st) == -1)
			goto ruleset_fail;
		if (S_ISDIR(st.st_mode) && allow_directory(ruleset, caps->roots + (int)i,
		    LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR) == -1)
			goto ruleset_fail;
	}
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
	/* The image input and each file setting are only read: no job asks
	 * them for zero ranges, so they get no CAP_FSYNC for the sync that
	 * would precede the host's hole report.
	 */
	{
		const int read_only[] = { caps->input, caps->settings[0],
		    caps->settings[1] };

		for (unsigned i = 0; i < sizeof(read_only) / sizeof(read_only[0]);
		    i++) {
			if (read_only[i] == -1)
				continue;
			cap_rights_init(&rights, CAP_READ, CAP_SEEK, CAP_FSTAT,
			    CAP_FCNTL);
			if (restrict_descriptor(read_only[i], &rights,
			    CAP_FCNTL_GETFL) == -1)
				goto freebsd_fail;
		}
	}
	/* The three writable roles share file rights and differ only in what
	 * they may do to names: destination makes directories, scratch
	 * removes its files, and staging does neither.
	 */
	{
		const struct {
			int	 fd;
			bool	 make_directories;
			bool	 remove_files;
		} writable[] = {
			{ caps->staging, false, false },
			{ caps->destination, true, false },
			{ caps->scratch, false, true }
		};

		for (unsigned i = 0; i < sizeof(writable) / sizeof(writable[0]); i++) {
			if (writable[i].fd == -1)
				continue;
			cap_rights_init(&rights, CAP_READ, CAP_WRITE, CAP_LOOKUP,
			    CAP_CREATE, CAP_FSTAT, CAP_SEEK, CAP_FSYNC, CAP_FTRUNCATE,
			    CAP_FCNTL);
			if (writable[i].make_directories)
				cap_rights_set(&rights, CAP_MKDIRAT);
			if (writable[i].remove_files)
				cap_rights_set(&rights, CAP_UNLINKAT);
			if (restrict_descriptor(writable[i].fd, &rights,
			    CAP_FCNTL_GETFL) == -1)
				goto freebsd_fail;
		}
	}
	for (unsigned i = 0; i < caps->root_count; i++) {
		cap_rights_init(&rights, CAP_READ, CAP_LOOKUP, CAP_FSTAT, CAP_SEEK,
		    CAP_FCNTL);
		if (restrict_descriptor(caps->roots + (int)i, &rights, CAP_FCNTL_GETFL) == -1)
			goto freebsd_fail;
	}
	/* A handed-over file is read, written and resized through its
	 * descriptor. Its own record answers which ranges are zero, so the
	 * host is never asked and the spool, which is never flushed, gets no
	 * CAP_FSYNC; the raw intermediate keeps it for the writer's flush of
	 * its sink.
	 */
	{
		const struct {
			int	 fd;
			bool	 flushed;
		} handed_over[] = { { caps->raw, true }, { caps->spool, false } };

		for (unsigned i = 0; i < 2; i++) {
			if (handed_over[i].fd == -1)
				continue;
			cap_rights_init(&rights, CAP_READ, CAP_WRITE, CAP_SEEK,
			    CAP_FSTAT, CAP_FTRUNCATE, CAP_FCNTL);
			if (handed_over[i].flushed)
				cap_rights_set(&rights, CAP_FSYNC);
			if (restrict_descriptor(handed_over[i].fd, &rights,
			    CAP_FCNTL_GETFL) == -1)
				goto freebsd_fail;
		}
	}
	/* The manifest is written, resized and flushed through its
	 * descriptor, never read.
	 */
	if (caps->manifest != -1) {
		cap_rights_init(&rights, CAP_WRITE, CAP_SEEK, CAP_FSTAT,
		    CAP_FTRUNCATE, CAP_FSYNC, CAP_FCNTL);
		if (restrict_descriptor(caps->manifest, &rights,
		    CAP_FCNTL_GETFL) == -1)
			goto freebsd_fail;
	}
	{
		const int channels[] = { caps->job, caps->control, caps->data, caps->cancel };

		/* A stream sink's identity is queried on the data channel. */
		for (unsigned i = 0; i < 4; i++) {
			if (channels[i] == -1)
				continue;
			cap_rights_init(&rights, i == 0 || i == 3 ? CAP_READ : CAP_WRITE);
			if (i == 2)
				cap_rights_set(&rights, CAP_FSTAT);
			if (restrict_descriptor(channels[i], &rights, 0) == -1)
				goto freebsd_fail;
		}
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
