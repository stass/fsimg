/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "cli.h"
#include "host.h"
#include "internal.h"
#include "private.h"
#include "publish.h"

/* What a private directory is called: the role in its name,
 * .fsimg-ROLE-RANDOM, and the noun errors use for the directory it is made
 * in.
 */
typedef struct private_kind {
	const char	*role;
	const char	*parent_noun;
} private_kind_t;

/* What a staged output is called: its private directory, its staged file
 * there, and the noun errors use for its final name.
 */
struct fsimg_cli_staged_kind {
	private_kind_t	 directory;
	const char	*file;
	const char	*final_noun;
};
typedef struct fsimg_cli_staged_kind staged_kind_t;

static const private_kind_t scratch_kind = {
	.role = "scratch", .parent_noun = "scratch directory"
};
static const staged_kind_t image_kind = {
	.directory = { .role = "output", .parent_noun = "output directory" },
	.file = "image", .final_noun = "output"
};
static const staged_kind_t manifest_kind = {
	.directory = { .role = "mtree", .parent_noun = "--mtree directory" },
	.file = "manifest", .final_noun = "--mtree file"
};

static fsimg_status_t host_error(fsimg_error_t *, const char *);
static fsimg_status_t path_error(fsimg_error_t *, const char *, const char *);
static fsimg_status_t kind_error(fsimg_error_t *, int, const char *,
    const char *);
static fsimg_status_t split_output(const char *, char *, char *, fsimg_error_t *);
static fsimg_status_t private_directory(int, const char *,
    const private_kind_t *, char *, int *, bool *, fsimg_error_t *);
static fsimg_status_t fresh_refused(fsimg_error_t *, fsimg_host_privacy_t,
    const char *, const char *);
static fsimg_status_t reject_ancestor(int, int, const char *, fsimg_error_t *);
static fsimg_status_t prepare_scratch(fsimg_cli_options_t *, fsimg_cli_paths_t *,
    fsimg_error_t *);
static fsimg_status_t open_input(fsimg_cli_options_t *, fsimg_error_t *);
static fsimg_status_t open_contents(int, const fsimg_cli_operand_t *, int,
    int *, fsimg_error_t *);
static unsigned operand_roots(const fsimg_cli_operand_t *);
static uint64_t worker_descriptors(const fsimg_worker_request_t *, unsigned);
static fsimg_status_t hold_descriptor(int, int, const char *, const char *, int *,
    fsimg_error_t *);
static fsimg_status_t open_settings(fsimg_cli_options_t *, fsimg_error_t *);
static int open_nofollow(int, const char *, bool);
static fsimg_status_t open_operand(int, const fsimg_cli_operand_t *, int,
    int *, fsimg_error_t *);
static fsimg_status_t open_roots(fsimg_cli_options_t *, fsimg_cli_paths_t *,
    fsimg_error_t *);
static fsimg_status_t generate_identifiers(fsimg_cli_options_t *, fsimg_error_t *);
static bool random_uuid(unsigned char [static 16]);
static fsimg_status_t prepare_staging(fsimg_cli_options_t *, fsimg_cli_paths_t *,
    fsimg_error_t *);
static fsimg_status_t handed_over_error(fsimg_error_t *, const char *,
    const char *);
static fsimg_status_t handed_over_file(int, const char *, int *,
    fsimg_error_t *);
static fsimg_status_t prepare_raw(fsimg_cli_options_t *, fsimg_error_t *);
static fsimg_status_t prepare_spool(fsimg_worker_request_t *, fsimg_error_t *);
static fsimg_status_t privacy_error(fsimg_error_t *, fsimg_host_privacy_t,
    const char *);
static void close_held(int *, int *);
static fsimg_status_t prepare_mtree(fsimg_cli_options_t *, fsimg_cli_paths_t *,
    fsimg_error_t *);
static fsimg_status_t recheck_mtree(fsimg_cli_options_t *, fsimg_cli_paths_t *,
    fsimg_error_t *);
static fsimg_status_t stage_output(fsimg_cli_staged_t *,
    const staged_kind_t *, const char *, fsimg_error_t *);
static fsimg_status_t final_name_free(const fsimg_cli_staged_t *,
    fsimg_error_t *);
static fsimg_status_t publish_staged(const fsimg_cli_options_t *,
    fsimg_cli_staged_t *, uint64_t, const volatile sig_atomic_t *,
    fsimg_error_t *);
static int clean_staged(fsimg_cli_staged_t *, bool);
static fsimg_status_t prepare_destination(fsimg_cli_options_t *,
    fsimg_cli_paths_t *, fsimg_error_t *);
static int clean_scratch(int);
static bool publication_cancelled(void *);

typedef struct publication_cancel {
	const volatile sig_atomic_t *interrupted;
	uint64_t	 deadline;
} publication_cancel_t;

uint64_t
fsimg_cli_clock(void)
{
	struct timespec time;

	if (clock_gettime(CLOCK_MONOTONIC, &time) == -1)
		return (0);
	return ((uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000);
}

static fsimg_status_t
host_error(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, errno == EEXIST ? FSIMG_EEXISTS : FSIMG_EIO,
	    errno, FSIMG_OP_IO, message));
}

static fsimg_status_t
path_error(fsimg_error_t *error, const char *message, const char *path)
{
	fsimg_status_t status;

	status = host_error(error, message);
	fsimg_error_path(error, path);
	return (status);
}

/* A host object of a kind the command refuses, such as a symbolic link or
 * a FIFO where a directory or regular file is required, is unsupported, as
 * the library's scanner reports it, naming path. os_error is the errno
 * that revealed the kind (ELOOP from a no-follow open), or zero when its
 * identity did.
 */
static fsimg_status_t
kind_error(fsimg_error_t *error, int os_error, const char *message,
    const char *path)
{
	fsimg_status_t status;

	status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, os_error,
	    FSIMG_OP_IO, message);
	fsimg_error_path(error, path);
	return (status);
}

static fsimg_status_t
split_output(const char *path, char *parent, char *name, fsimg_error_t *error)
{
	const char *base;
	size_t prefix, length;

	length = strlen(path);
	if (length == 0 || length >= PATH_MAX)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "output pathname is empty or exceeds the host path bound"));
	base = strrchr(path, '/');
	if (base == NULL) {
		strcpy(parent, ".");
		base = path;
	} else {
		prefix = base == path ? 1 : (size_t)(base - path);
		bcopy(path, parent, prefix);
		parent[prefix] = '\0';
		base++;
	}
	if (base[0] == '\0' || strlen(base) > NAME_MAX ||
	    strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "output requires a nonempty host basename"));
	strcpy(name, base);
	return (FSIMG_OK);
}

/*
 * A fresh private directory of kind beneath parent, the directory at
 * parent_path, under an unused random name (fsimg_host_private_mkdir).
 */
static fsimg_status_t
private_directory(int parent, const char *parent_path,
    const private_kind_t *kind, char *name, int *out, bool *created,
    fsimg_error_t *error)
{
	fsimg_host_privacy_t verdict;
	uint64_t random[2];
	char message[128];
	int saved;

	for (unsigned attempt = 0; attempt < 128; attempt++) {
		if (fsimg_host_random(random, sizeof(random)) == -1)
			return (host_error(error, "private directory randomness failed"));
		snprintf(name, 96, ".fsimg-%s-%016llx%016llx", kind->role,
		    (unsigned long long)random[0], (unsigned long long)random[1]);
		switch (fsimg_host_private_mkdir(parent, name, out, &verdict,
		    created)) {
		case FSIMG_HOST_FRESH_CREATED:
			return (FSIMG_OK);
		case FSIMG_HOST_FRESH_TAKEN:
			continue;
		case FSIMG_HOST_FRESH_REFUSED:
			return (fresh_refused(error, verdict, kind->parent_noun,
			    parent_path));
		case FSIMG_HOST_FRESH_FAILED:
			saved = errno;
			snprintf(message, sizeof(message),
			    "cannot create a private directory in the %s",
			    kind->parent_noun);
			errno = saved;
			return (path_error(error, message, parent_path));
		}
	}
	return (fsimg_error_set(error, FSIMG_EEXISTS, EEXIST,
	    FSIMG_OP_IO, "private directory name attempts exhausted"));
}

/*
 * A new directory beneath parent_noun at parent_path that the
 * private-directory rule refused. The cause lies in that parent, so the
 * error names it.
 */
static fsimg_status_t
fresh_refused(fsimg_error_t *error, fsimg_host_privacy_t verdict,
    const char *parent_noun, const char *parent_path)
{
	fsimg_status_t status;
	char message[128];

	snprintf(message, sizeof(message), "%s %s", parent_noun,
	    fsimg_host_fresh_refusal(verdict));
	status = fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IO, message);
	fsimg_error_path(error, parent_path);
	return (status);
}

/* Compare opened identities while walking toward the host root, failing
 * with message when ancestor is descendant or one of its ancestors. No
 * pathname supplied by a worker participates. Scratch and output are
 * checked in both directions because scratch deletion authority must never
 * contain the output tree.
 */
static fsimg_status_t
reject_ancestor(int descendant, int ancestor, const char *message,
    fsimg_error_t *error)
{
	fsimg_host_file_t target, current, parent;
	fsimg_status_t status;
	int fd, next, saved;

	if (fsimg_host_file(ancestor, &target) == -1)
		return (host_error(error, "private directory identity query failed"));
	fd = fsimg_host_dup(descendant);
	if (fd == -1)
		return (host_error(error, "private directory ancestry open failed"));
	status = FSIMG_OK;
	for (unsigned depth = 0;; depth++) {
		if (fsimg_host_file(fd, &current) == -1) {
			status = host_error(error, "private directory ancestry query failed");
			break;
		}
		if (current.device == target.device && current.inode == target.inode) {
			status = fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IO, message);
			break;
		}
		if (depth == PATH_MAX) {
			status = fsimg_error_set(error, FSIMG_ELIMIT, 0,
			    FSIMG_OP_IO, "private directory ancestry exceeds host path bound");
			break;
		}
		next = openat(fd, "..", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (next == -1) {
			status = host_error(error, "private directory parent open failed");
			break;
		}
		if (fsimg_host_file(next, &parent) == -1) {
			saved = errno;
			close(next);
			errno = saved;
			status = host_error(error, "private directory parent query failed");
			break;
		}
		close(fd);
		fd = next;
		if (current.device == parent.device && current.inode == parent.inode)
			break;
	}
	close(fd);
	return (status);
}

/* Prepare scratch before creating output. A -T path that would exist only
 * inside the new extraction root cannot acquire cleanup authority there.
 */
static fsimg_status_t
prepare_scratch(fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	fsimg_status_t status;

	if (strcmp(options->scratch, "-") == 0)
		return (FSIMG_OK);
	if (strlen(options->scratch) + sizeof(paths->scratch_name) + 1 >= PATH_MAX)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_IO, "scratch directory leaves no room for a private pathname"));
	paths->scratch_parent = open(options->scratch, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (paths->scratch_parent == -1)
		return (path_error(error, "cannot open scratch directory", options->scratch));
	status = private_directory(paths->scratch_parent, options->scratch,
	    &scratch_kind, paths->scratch_name, &request->scratch_fd,
	    &paths->scratch_created, error);
	if (paths->scratch_created)
		snprintf(paths->retained_scratch, sizeof(paths->retained_scratch),
		    "%s/%s", options->scratch, paths->scratch_name);
	return (status);
}

/* The image of a read mode: a regular file, or standard input under -f -,
 * which the coordinator relays to the worker unread and which must not be
 * a terminal. A volume range given by --offset and --length addresses the
 * disk, which a container or compression can make longer than the file,
 * so only the worker, which peels them, can check that the range fits;
 * the selected volume then bounds what extraction may write unless
 * -l output says otherwise. Every read job with scratch receives a spool,
 * since only the worker learns whether the input needs one.
 */
static fsimg_status_t
open_input(fsimg_cli_options_t *options, fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	fsimg_host_file_t identity;
	fsimg_status_t status;

	status = prepare_spool(request, error);
	if (status != FSIMG_OK)
		return (status);
	if ((request->flags & FSIMG_JOB_STDIN) != 0) {
		if (isatty(STDIN_FILENO))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IO,
			    "refusing to read an image from a terminal"));
		return (FSIMG_OK);
	}
	request->input_fd = fsimg_host_source_open(AT_FDCWD, options->image, false);
	if (request->input_fd == -1 && errno == ELOOP)
		return (kind_error(error, ELOOP, "image is a symbolic link",
		    options->image));
	if (request->input_fd == -1 && errno == ENXIO)
		return (kind_error(error, ENXIO, "image is not a regular file",
		    options->image));
	if (request->input_fd == -1)
		return (path_error(error, "cannot open image", options->image));
	if (fsimg_host_file(request->input_fd, &identity) == -1)
		return (host_error(error, "image identity query failed"));
	if (identity.kind != FSIMG_HOST_FILE)
		return (kind_error(error, 0, "image is not a regular file",
		    options->image));
	return (FSIMG_OK);
}

/*
 * Open a create's file settings (specs/design-layers.md#file-settings-and-
 * sections) before the worker starts, each -o path resolved against the
 * working directory and opened as a command-line operand is (open_nofollow):
 * no-follow even before a trailing "/" or "/.", nonblocking and without a
 * controlling terminal, read-only, and refused as unsupported unless a
 * regular file. The worker receives the descriptor alone, never the name.
 * Only creation and its plan reach here; reading refuses the keys while
 * parsing.
 */
static fsimg_status_t
open_settings(fsimg_cli_options_t *options, fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	fsimg_host_file_t identity;
	const char *path;
	int *fd;

	for (unsigned i = 0; i < FSIMG_FILE_SETTINGS; i++) {
		path = options->setting_paths[i];
		if (path == NULL)
			continue;
		fd = &request->setting_fds[i];
		*fd = open_nofollow(AT_FDCWD, path, false);
		if (*fd == -1 && errno == ELOOP)
			return (kind_error(error, ELOOP,
			    "file setting is a symbolic link", path));
		if (*fd == -1 && errno == ENXIO)
			return (kind_error(error, ENXIO,
			    "file setting is not a regular file", path));
		if (*fd == -1)
			return (path_error(error, "cannot open file setting", path));
		if (fsimg_host_file(*fd, &identity) == -1)
			return (path_error(error, "file setting identity query failed",
			    path));
		if (identity.kind != FSIMG_HOST_FILE)
			return (kind_error(error, 0,
			    "file setting is not a regular file", path));
	}
	return (FSIMG_OK);
}

/*
 * Copy path into trimmed, a PATH_MAX buffer, without its trailing slashes
 * and "." components, setting *removed when there were any, since a
 * trailing slash names a directory. O_NOFOLLOW covers only the final
 * component, and a path ending in "/" or "/." makes the kernel resolve a
 * symbolic link before that component; once they are removed, "link/."
 * names the link itself. A path left with no component names the base
 * directory itself (".") or the root ("/"). Fails with ENAMETOOLONG.
 */
static bool
trim_final(const char *path, char *trimmed, bool *removed)
{
	size_t length;

	length = strlen(path);
	if (length >= PATH_MAX) {
		errno = ENAMETOOLONG;
		return (false);
	}
	bcopy(path, trimmed, length + 1);
	*removed = false;
	for (;;) {
		if (length > 1 && trimmed[length - 1] == '/')
			length--;
		else if (length == 1 && trimmed[0] == '.')
			length = 0;
		else if (length >= 2 && trimmed[length - 1] == '.' &&
		    trimmed[length - 2] == '/')
			length--;
		else
			break;
		trimmed[length] = '\0';
		*removed = true;
	}
	if (length == 0)
		bcopy(".", trimmed, sizeof("."));
	return (true);
}

/*
 * Whether path beneath base, trimmed by trim_final, names an existing
 * object, which *st describes: with follow what a symbolic link there
 * reaches, otherwise the link itself. It leaves errno unchanged.
 */
static bool
stat_at(int base, const char *path, bool follow, struct stat *st)
{
	char trimmed[PATH_MAX];
	int saved;
	bool removed, found;

	saved = errno;
	found = trim_final(path, trimmed, &removed) &&
	    fstatat(base, trimmed, st, follow ? 0 : AT_SYMLINK_NOFOLLOW) == 0;
	errno = saved;
	return (found);
}

/*
 * The failure of opening the -C directory path, which failed with
 * open_errno. Without -E the open follows symbolic links, as tar's -C
 * does; under -E (follow false) it does not, and a symbolic link fails
 * with ELOOP on every host (fsimg_host_source_open). The failure alone
 * cannot say where it arose: ENOTDIR and ENXIO come from an existing object
 * of another kind or, for ENOTDIR, a non-directory earlier in the path, and
 * ELOOP from the final link or a loop earlier in the path. Looking the
 * object up tells them apart: a final link under -E, or an existing object
 * of another kind judged on the object the open met, is unsupported; any
 * other failure is an open failure. The lookup only classifies the
 * failure; nothing is opened or trusted on it.
 */
static fsimg_status_t
change_directory_error(fsimg_error_t *error, const char *path, bool follow,
    int open_errno)
{
	struct stat st;

	if (!follow && open_errno == ELOOP &&
	    stat_at(AT_FDCWD, path, false, &st) && S_ISLNK(st.st_mode))
		return (kind_error(error, ELOOP,
		    "-C directory is a symbolic link", path));
	if ((open_errno == ENOTDIR || open_errno == ENXIO) &&
	    stat_at(AT_FDCWD, path, follow, &st) && !S_ISDIR(st.st_mode))
		return (kind_error(error, open_errno,
		    "-C directory is not a directory", path));
	errno = open_errno;
	return (path_error(error, "cannot open -C directory", path));
}

/*
 * The no-follow scanned-object open of a command-line path, trimmed by
 * trim_final; a trailing slash stays a requirement for a directory. A
 * refused link fails with ELOOP on every host (fsimg_host_source_open).
 */
static int
open_nofollow(int base, const char *path, bool directory)
{
	char trimmed[PATH_MAX];
	bool removed;

	if (!trim_final(path, trimmed, &removed))
		return (-1);
	if (removed)
		directory = true;
	return (fsimg_host_source_open(base, trimmed, directory));
}

/*
 * Open one create operand with the no-follow, nonblocking,
 * no-controlling-terminal open used for every scanned object, so a symbolic
 * link operand fails and a FIFO cannot block. Only a directory or a regular
 * file becomes a root, and only a regular file a manifest. The descriptor
 * is moved above the worker's descriptor layout, at least first_free, so
 * starting the worker passes it without another copy.
 */
static fsimg_status_t
open_operand(int directory, const fsimg_cli_operand_t *operand, int first_free,
    int *out, fsimg_error_t *error)
{
	fsimg_host_file_t identity;
	int fd, saved;
	bool kind_valid;

	fd = open_nofollow(operand->host_path[0] == '/' ? AT_FDCWD : directory,
	    operand->host_path, false);
	if (fd == -1) {
		if (errno == ELOOP)
			return (kind_error(error, ELOOP, operand->manifest ?
			    "manifest is a symbolic link" : "operand is a symbolic link",
			    operand->host_path));
		if (errno == ENXIO)
			return (kind_error(error, ENXIO, operand->manifest ?
			    "manifest is not a regular file" :
			    "operand is not a regular file or directory",
			    operand->host_path));
		return (path_error(error, operand->manifest ?
		    "cannot open manifest" : "cannot open operand", operand->host_path));
	}
	if (fsimg_host_file(fd, &identity) == -1) {
		saved = errno;
		close(fd);
		errno = saved;
		return (path_error(error, operand->manifest ?
		    "manifest identity query failed" : "operand identity query failed",
		    operand->host_path));
	}
	kind_valid = identity.kind == FSIMG_HOST_FILE ||
	    (!operand->manifest && identity.kind == FSIMG_HOST_DIRECTORY);
	if (!kind_valid) {
		close(fd);
		return (kind_error(error, 0, operand->manifest ?
		    "manifest is not a regular file" :
		    "operand is not a regular file or directory", operand->host_path));
	}
	return (hold_descriptor(fd, first_free, operand->manifest ?
	    "cannot hold manifest" : "cannot hold operand", operand->host_path,
	    out, error));
}

/*
 * A manifest's content directory: the -C directory in force, opened again
 * as an operand of "." is, beneath which the worker reads a file's bytes
 * only when the manifest's entry needs them. Only a manifest given with -C
 * has one (operand_roots).
 */
static fsimg_status_t
open_contents(int directory, const fsimg_cli_operand_t *operand,
    int first_free, int *out, fsimg_error_t *error)
{
	int fd;

	fd = fsimg_host_source_open(directory, ".", true);
	if (fd == -1)
		return (path_error(error, "cannot open the manifest's content directory",
		    operand->directory));
	return (hold_descriptor(fd, first_free,
	    "cannot hold the manifest's content directory", operand->directory,
	    out, error));
}

/* The root descriptors an operand holds: one, and for a manifest with a
 * -C directory in force a second, its content directory. A manifest
 * without one has no content directory, so the worker's writer refuses
 * every entry that needs host bytes.
 */
static unsigned
operand_roots(const fsimg_cli_operand_t *operand)
{
	return (operand->manifest && operand->directory != NULL ? 2 : 1);
}

/*
 * The descriptors a create or plan job's worker needs for a flat source
 * tree: its root descriptors, scratch unless it is disabled, the staging
 * directory and raw intermediate when the job needs them, and two working
 * ones. Scanning a flat root uses those two for a directory and the entry
 * read from it; copying, the staged image and a source file. A deeper tree
 * holds one more per directory level while it is scanned, and one more
 * while a file below the top level is copied, which the worker refuses
 * against the same limit: it charges every descriptor it holds, these
 * included, from the start.
 */
static uint64_t
worker_descriptors(const fsimg_worker_request_t *request, unsigned roots)
{
	unsigned settings;
	bool scratch, staging, raw;

	scratch = request->scratch_fd != -1;
	staging = fsimg_job_needs_staging(request);
	raw = fsimg_job_needs_raw(request);
	settings = 0;
	for (unsigned i = 0; i < FSIMG_FILE_SETTINGS; i++)
		settings += request->setting_fds[i] != -1 ? 1 : 0;
	return ((uint64_t)roots + (scratch ? 1 : 0) + (staging ? 1 : 0) +
	    (raw ? 1 : 0) + settings + 2);
}

/* Move fd to the lowest descriptor at or above first_free, close-on-exec,
 * and close the original on every outcome; a failure reports message and
 * the object's name.
 */
static fsimg_status_t
hold_descriptor(int fd, int first_free, const char *message, const char *name,
    int *out, fsimg_error_t *error)
{
	int saved;

	*out = fcntl(fd, F_DUPFD_CLOEXEC, first_free);
	saved = errno;
	close(fd);
	if (*out == -1) {
		errno = saved;
		return (path_error(error, message, name));
	}
	return (FSIMG_OK);
}

/*
 * Open every create operand before the worker starts; the opened roots are
 * the worker's whole source authority, so it never receives a -C directory
 * or a name to resolve. Each operand is opened relative to the -C directory
 * that precedes it, or the working directory, and an absolute operand as
 * given. A host operand is one root descriptor. A manifest is one root
 * descriptor for the manifest file, path FSIMG_JOB_MANIFEST, followed,
 * when a -C is in force, by one for its content directory, path
 * FSIMG_JOB_CONTENTS; the worker adopts them as one source root. The
 * operands must leave the worker room within the fds limit
 * (worker_descriptors), so an oversized command line fails here, before
 * the worker starts.
 */
static fsimg_status_t
open_roots(fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	const fsimg_cli_operand_t *operand;
	const char *directory_name;
	unsigned count, slot;
	fsimg_status_t status;
	uint64_t needed;
	int directory, first_free;

	count = 0;
	for (unsigned i = 0; i < options->operand_count; i++)
		count += operand_roots(&options->operands[i]);
	needed = worker_descriptors(request, count);
	if (needed > request->limits.max_open_files)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_DESCRIPTORS,
		    request->limits.max_open_files, needed));
	paths->root_fds = calloc(count, sizeof(*paths->root_fds));
	paths->root_paths = calloc(count, sizeof(*paths->root_paths));
	if (paths->root_fds == NULL || paths->root_paths == NULL)
		return (fsimg_error_set(error, FSIMG_ENOMEM, ENOMEM,
		    FSIMG_OP_IO, "operand descriptor table allocation failed"));
	for (unsigned i = 0; i < count; i++)
		paths->root_fds[i] = -1;
	/* Published before any open, so cleanup closes the roots opened before
	 * a later operand fails.
	 */
	request->root_fds = paths->root_fds;
	request->root_paths = paths->root_paths;
	request->root_count = count;
	first_free = FSIMG_CHILD_ROOTS + (int)count;
	directory = AT_FDCWD;
	directory_name = NULL;
	status = FSIMG_OK;
	slot = 0;
	for (unsigned i = 0; i < options->operand_count && status == FSIMG_OK; i++) {
		operand = &options->operands[i];
		/* Operands after one -C borrow the same argv string, so a new
		 * pointer, not new text, marks the next -C: two -C words naming
		 * the same path are opened separately, each when it applies.
		 */
		if (operand->directory != directory_name) {
			if (directory != AT_FDCWD)
				close(directory);
			directory = open(operand->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			directory_name = operand->directory;
			if (directory == -1) {
				status = change_directory_error(error,
				    operand->directory, true, errno);
				break;
			}
		}
		/* Each slot's path is set even when its open fails; the loop
		 * then ends, and cleanup reads only the descriptors.
		 */
		status = open_operand(directory, operand, first_free,
		    &paths->root_fds[slot], error);
		paths->root_paths[slot] = operand->manifest ? FSIMG_JOB_MANIFEST :
		    operand->image_path;
		if (operand_roots(operand) == 2) {
			paths->root_paths[slot + 1] = FSIMG_JOB_CONTENTS;
			if (status == FSIMG_OK)
				status = open_contents(directory, operand,
				    first_free, &paths->root_fds[slot + 1], error);
		}
		slot += operand_roots(operand);
	}
	if (directory != AT_FDCWD && directory != -1)
		close(directory);
	return (status);
}

/* Without -R every volume receives a random serial, a format storing a
 * UUID a random version 4 UUID unless one is given, a partition scheme
 * random identifiers for those its settings leave unset: version 4 disk
 * and partition GUIDs for GPT, a 32-bit disk identifier for MBR, and a DMG
 * or VHD container a version 4 identifier unless dmg:uuid or vhd:uuid
 * gives one. Under -R the library derives them from the epoch instead.
 */
static fsimg_status_t
generate_identifiers(fsimg_cli_options_t *options, fsimg_error_t *error)
{
	static const unsigned char nil[16];
	fsimg_write_options_t *write = &options->request.write;
	fsimg_layer_options_t *layers = &options->request.layers;
	const fsimg_stack_slot_t *scheme, *container;

	if ((write->flags & FSIMG_WRITE_REPRODUCIBLE) != 0)
		return (FSIMG_OK);
	if (fsimg_host_random(&write->volume_serial, sizeof(write->volume_serial)) == -1 ||
	    (options->random_uuid && !random_uuid(write->volume_uuid)))
		return (host_error(error, "volume identifier generation failed"));
	container = &options->request.stack.slots[FSIMG_LAYER_CONTAINER];
	if (container->state == FSIMG_SLOT_NAMED &&
	    ((strcmp(container->name, "dmg") == 0 &&
	    bcmp(layers->dmg_uuid, nil, sizeof(nil)) == 0 &&
	    !random_uuid(layers->dmg_uuid)) ||
	    (strcmp(container->name, "vhd") == 0 &&
	    bcmp(layers->vhd_uuid, nil, sizeof(nil)) == 0 &&
	    !random_uuid(layers->vhd_uuid))))
		return (host_error(error, "container identifier generation failed"));
	scheme = &options->request.stack.slots[FSIMG_LAYER_SCHEME];
	if (scheme->state != FSIMG_SLOT_NAMED)
		return (FSIMG_OK);
	if (strcmp(scheme->name, "gpt") == 0 &&
	    ((bcmp(layers->gpt_disk_guid, nil, sizeof(nil)) == 0 &&
	    !random_uuid(layers->gpt_disk_guid)) ||
	    (bcmp(layers->gpt_guid, nil, sizeof(nil)) == 0 &&
	    !random_uuid(layers->gpt_guid))))
		return (host_error(error, "disk identifier generation failed"));
	if (strcmp(scheme->name, "mbr") == 0 && !layers->mbr_id_set) {
		if (fsimg_host_random(&layers->mbr_id, sizeof(layers->mbr_id)) == -1)
			return (host_error(error, "disk identifier generation failed"));
		layers->mbr_id_set = true;
	}
	return (FSIMG_OK);
}

/* A random RFC 4122 version 4 UUID in textual byte order. */
static bool
random_uuid(unsigned char uuid[static 16])
{
	if (fsimg_host_random(uuid, 16) == -1)
		return (false);
	uuid[6] = (unsigned char)((uuid[6] & 0x0f) | 0x40);
	uuid[8] = (unsigned char)((uuid[8] & 0x3f) | 0x80);
	return (true);
}

/*
 * A staged output's private directory beside its final name path, which
 * must not exist yet. A failure can leave the record partly filled; it is
 * still fsimg_cli_cleanup's to clean.
 */
static fsimg_status_t
stage_output(fsimg_cli_staged_t *staged, const staged_kind_t *kind,
    const char *path, fsimg_error_t *error)
{
	char parent_path[PATH_MAX], message[96];
	fsimg_status_t status;
	int saved;

	staged->kind = kind;
	staged->path = path;
	status = split_output(path, parent_path, staged->name, error);
	if (status != FSIMG_OK)
		return (status);
	/* Room for the retained name parent_path/directory_name/file. */
	if (strlen(parent_path) + sizeof(staged->directory_name) +
	    strlen(kind->file) + 2 >= PATH_MAX) {
		snprintf(message, sizeof(message),
		    "%s leaves no room for a private pathname",
		    kind->directory.parent_noun);
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_IO,
		    message));
	}
	staged->parent = open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (staged->parent == -1) {
		saved = errno;
		snprintf(message, sizeof(message), "cannot open %s",
		    kind->directory.parent_noun);
		errno = saved;
		return (path_error(error, message, parent_path));
	}
	status = final_name_free(staged, error);
	if (status != FSIMG_OK)
		return (status);
	status = private_directory(staged->parent, parent_path, &kind->directory,
	    staged->directory_name, &staged->directory, &staged->created, error);
	if (staged->created)
		snprintf(staged->retained, sizeof(staged->retained), "%s/%s",
		    parent_path, staged->directory_name);
	return (status);
}

/* Fail unless the staged output's final name is free, never following a
 * symbolic link there.
 */
static fsimg_status_t
final_name_free(const fsimg_cli_staged_t *staged, fsimg_error_t *error)
{
	struct stat st;
	char message[96];
	int saved;

	if (fstatat(staged->parent, staged->name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		snprintf(message, sizeof(message), "cannot create %s",
		    staged->kind->final_noun);
		errno = EEXIST;
		return (path_error(error, message, staged->path));
	}
	if (errno == ENOENT)
		return (FSIMG_OK);
	saved = errno;
	snprintf(message, sizeof(message), "cannot inspect %s",
	    staged->kind->final_noun);
	errno = saved;
	return (path_error(error, message, staged->path));
}

/* A creation's staged image. The worker creates "image" in the private
 * directory only after planning; only the coordinator publishes it.
 */
static fsimg_status_t
prepare_staging(fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = stage_output(&paths->image, &image_kind, options->image, error);
	options->request.output_fd = paths->image.directory;
	return (status);
}

/*
 * The raw intermediate of a creation that writes a container, a compressed
 * image or a stream (specs/design-layers.md#handed-over-files): created
 * exclusively with mode 0600 beside the staged image, or in the scratch
 * tree for a stream, and unlinked at once, so the worker holds a file no
 * name reaches and nothing is left to clean up on any outcome.
 */
static fsimg_status_t
prepare_raw(fsimg_cli_options_t *options, fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	int directory;
	bool stream;

	if (!fsimg_job_needs_raw(request))
		return (FSIMG_OK);
	stream = (request->flags & FSIMG_JOB_STDOUT) != 0;
	directory = stream ? request->scratch_fd : request->output_fd;
	if (directory == -1)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IO,
		    "an image on standard output needs scratch for its raw disk"));
	return (handed_over_file(directory, "raw", &request->raw_fd, error));
}

/* The spool of a read job (specs/design-layers.md#handed-over-files), in
 * the scratch tree; without scratch (-T -) there is none.
 */
static fsimg_status_t
prepare_spool(fsimg_worker_request_t *request, fsimg_error_t *error)
{
	if (request->scratch_fd == -1)
		return (FSIMG_OK);
	return (handed_over_file(request->scratch_fd, "spool", &request->spool_fd,
	    error));
}

/*
 * A handed-over file: created exclusively with mode 0600 under a random
 * name in directory and unlinked at once, so the worker holds a file no
 * name reaches and nothing is left to clean up on any outcome. role names
 * it ("raw" or "spool").
 */
static fsimg_status_t
handed_over_file(int directory, const char *role, int *fd,
    fsimg_error_t *error)
{
	uint64_t random[2];
	char name[96];
	int saved;

	for (unsigned attempt = 0; attempt < 128 && *fd == -1; attempt++) {
		if (fsimg_host_random(random, sizeof(random)) == -1)
			return (handed_over_error(error, role, "randomness failed"));
		snprintf(name, sizeof(name), ".fsimg-%s-%016llx%016llx", role,
		    (unsigned long long)random[0], (unsigned long long)random[1]);
		*fd = openat(directory, name, O_RDWR | O_CREAT | O_EXCL |
		    O_NOFOLLOW | O_CLOEXEC, 0600);
		if (*fd == -1 && errno != EEXIST)
			return (handed_over_error(error, role, "creation failed"));
	}
	if (*fd == -1) {
		errno = EEXIST;
		return (handed_over_error(error, role, "name attempts exhausted"));
	}
	if (unlinkat(directory, name, 0) == -1) {
		saved = errno;
		close(*fd);
		*fd = -1;
		errno = saved;
		return (handed_over_error(error, role, "unlink failed"));
	}
	return (FSIMG_OK);
}

/* A host failure naming the handed-over file by its role, errno intact. */
static fsimg_status_t
handed_over_error(fsimg_error_t *error, const char *role, const char *failure)
{
	char message[96];
	int saved;

	saved = errno;
	snprintf(message, sizeof(message), "%s file %s", role, failure);
	errno = saved;
	return (host_error(error, message));
}

/* An existing -E destination that fails the privacy rule. */
static fsimg_status_t
privacy_error(fsimg_error_t *error, fsimg_host_privacy_t verdict,
    const char *path)
{
	const char *message;
	fsimg_status_t status;

	status = FSIMG_EINVAL;
	switch (verdict) {
	case FSIMG_HOST_NOT_DIRECTORY:
		message = "extraction destination is not a directory";
		break;
	case FSIMG_HOST_FOREIGN_OWNER:
		message = "extraction destination is not owned by the effective user";
		break;
	case FSIMG_HOST_SHARED_WRITE:
		message = "extraction destination is writable by group or others";
		break;
	case FSIMG_HOST_EXTENDED_ACL:
		message = "extraction destination carries a non-trivial access control list";
		break;
	case FSIMG_HOST_NOT_EMPTY:
		message = "extraction destination is not empty";
		status = FSIMG_EEXISTS;
		break;
	default:
		message = "extraction destination is not private";
		break;
	}
	errno = 0;
	path_error(error, message, path);
	if (error != NULL) {
		error->status = status;
		error->os_error = 0;
	}
	return (status);
}

/*
 * The manifest of x --mtree, staged like a created image. The file
 * "manifest" is created exclusively with mode 0600 in the private
 * directory, and the worker receives only its descriptor, write-only; only
 * the coordinator publishes it, after a validated completion. It is
 * prepared before the destination, so a refused name leaves no extraction
 * directory behind.
 */
static fsimg_status_t
prepare_mtree(fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    fsimg_error_t *error)
{
	fsimg_cli_staged_t *staged = &paths->manifest;
	fsimg_status_t status;

	status = stage_output(staged, &manifest_kind, options->mtree, error);
	if (status != FSIMG_OK)
		return (status);
	options->request.manifest_fd = openat(staged->directory, staged->kind->file,
	    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (options->request.manifest_fd == -1)
		return (host_error(error, "staged manifest creation failed"));
	return (FSIMG_OK);
}

/* The --mtree name is checked again once the destination exists, so a
 * name the new destination took, such as --mtree=boot beside ./boot, fails
 * before anything is extracted rather than at publication. The fresh
 * destination is still empty then and is removed again, as a refused
 * private one is; an -E destination was never fsimg's to remove.
 */
static fsimg_status_t
recheck_mtree(fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = final_name_free(&paths->manifest, error);
	if (status != FSIMG_OK && !options->extract_here &&
	    fsimg_host_remove_directory(paths->destination_parent, options->stem,
	    options->request.output_fd)) {
		paths->extraction_prepared = false;
		paths->retained_destination[0] = '\0';
	}
	return (status);
}

/*
 * The extraction destination, proven private before the worker starts.
 * By default it is a fresh private directory named after the image stem
 * beneath the opened -C directory (fsimg_host_private_mkdir), so an
 * existing name of any kind fails. With -E it is the -C directory itself,
 * opened no-follow. Either way the opened descriptor alone must pass the
 * privacy rule, and the worker receives only it.
 */
static fsimg_status_t
prepare_destination(fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	fsimg_host_privacy_t verdict;
	const char *parent_path;
	fsimg_status_t status;
	int count;

	parent_path = options->directory == NULL ? "." : options->directory;
	if (options->extract_here) {
		request->output_fd = open_nofollow(AT_FDCWD, parent_path, true);
		if (request->output_fd == -1)
			return (change_directory_error(error, parent_path, false,
			    errno));
		if (fsimg_host_private_directory(request->output_fd, &verdict) == -1)
			return (path_error(error, "cannot inspect -C directory", parent_path));
		if (verdict != FSIMG_HOST_PRIVATE)
			return (privacy_error(error, verdict, parent_path));
		paths->extraction_prepared = true;
		snprintf(paths->retained_destination,
		    sizeof(paths->retained_destination), "%s", parent_path);
		return (FSIMG_OK);
	}

	paths->destination_parent = open(parent_path,
	    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (paths->destination_parent == -1)
		return (change_directory_error(error, parent_path, true, errno));
	count = snprintf(paths->retained_destination,
	    sizeof(paths->retained_destination), "%s/%s", parent_path, options->stem);
	if (count < 0 || (size_t)count >= sizeof(paths->retained_destination))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_IO, "extraction pathname exceeds host path bound"));
	/* A fresh destination is prepared exactly while it exists. */
	switch (fsimg_host_private_mkdir(paths->destination_parent, options->stem,
	    &request->output_fd, &verdict, &paths->extraction_prepared)) {
	case FSIMG_HOST_FRESH_CREATED:
		return (FSIMG_OK);
	case FSIMG_HOST_FRESH_REFUSED:
		status = fresh_refused(error, verdict, "-C directory", parent_path);
		break;
	default:
		status = path_error(error, "cannot create extraction directory",
		    paths->retained_destination);
		break;
	}
	if (!paths->extraction_prepared)
		paths->retained_destination[0] = '\0';
	return (status);
}

void
fsimg_cli_paths_init(fsimg_cli_paths_t *paths)
{
	bzero(paths, sizeof(*paths));
	paths->image.parent = paths->image.directory = -1;
	paths->manifest.parent = paths->manifest.directory = -1;
	paths->destination_parent = paths->scratch_parent = -1;
}

fsimg_status_t
fsimg_cli_prepare(fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	fsimg_status_t status;

	fsimg_cli_paths_init(paths);
	status = prepare_scratch(options, paths, error);
	if (status != FSIMG_OK)
		return (status);

	/* Inputs before outputs: a missing image or operand leaves no
	 * extraction directory or staging behind.
	 */
	switch (request->kind) {
	case FSIMG_JOB_CREATE:
	case FSIMG_JOB_PLAN:
		/* An image stream is binary; a terminal is no place for it. */
		if ((request->flags & FSIMG_JOB_STDOUT) != 0 &&
		    isatty(STDOUT_FILENO))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IO,
			    "refusing to write an image to a terminal"));
		status = open_settings(options, error);
		if (status == FSIMG_OK)
			status = open_roots(options, paths, error);
		if (status == FSIMG_OK)
			status = generate_identifiers(options, error);
		if (status == FSIMG_OK && fsimg_job_needs_staging(request))
			status = prepare_staging(options, paths, error);
		if (status == FSIMG_OK)
			status = prepare_raw(options, error);
		break;
	case FSIMG_JOB_EXTRACT:
		status = open_input(options, error);
		if (status == FSIMG_OK && options->mtree != NULL)
			status = prepare_mtree(options, paths, error);
		if (status == FSIMG_OK)
			status = prepare_destination(options, paths, error);
		if (status == FSIMG_OK && options->mtree != NULL)
			status = recheck_mtree(options, paths, error);
		break;
	default:
		status = open_input(options, error);
		break;
	}
	if (status != FSIMG_OK)
		return (status);

	if (request->output_fd != -1 && request->scratch_fd != -1) {
		status = reject_ancestor(request->output_fd, request->scratch_fd,
		    "scratch and output trees overlap", error);
		if (status != FSIMG_OK)
			return (status);
		status = reject_ancestor(request->scratch_fd, request->output_fd,
		    "scratch and output trees overlap", error);
	}
	/* The worker must not see the manifest beside the tree it fills. */
	if (status == FSIMG_OK && paths->manifest.directory != -1 &&
	    request->output_fd != -1)
		status = reject_ancestor(paths->manifest.directory, request->output_fd,
		    "--mtree file lies inside the extraction destination", error);
	return (status);
}

/* The request of cW's verification worker: the staged image, opened
 * read-only and no-follow through the staging descriptor, checked with the
 * ordinary verification under the image-reading profile, with the
 * creation's stack forced, an unnamed scheme or container forced empty,
 * and its limits and scratch.
 */
fsimg_status_t
fsimg_cli_verification_request(const fsimg_cli_options_t *options,
    const fsimg_cli_paths_t *paths, fsimg_worker_request_t *verify,
    fsimg_error_t *error)
{
	const fsimg_worker_request_t *create = &options->request;
	fsimg_host_file_t identity;

	*verify = *create;
	verify->kind = FSIMG_JOB_VERIFY;
	verify->flags = create->flags & (FSIMG_JOB_PAYLOAD | FSIMG_JOB_VERBOSE);
	/* Every slot is forced below, and so is the created compression,
	 * which the copied request names.
	 */
	verify->selection = "/";
	verify->root_fds = NULL;
	verify->root_paths = NULL;
	verify->root_count = 0;
	verify->output_fd = verify->raw_fd = verify->spool_fd = -1;
	/* The coordinator, not the verification, holds the file settings. */
	for (unsigned i = 0; i < FSIMG_FILE_SETTINGS; i++)
		verify->setting_fds[i] = -1;
	verify->offset = verify->length = 0;
	verify->selector = FSIMG_SELECT_AUTOMATIC;
	/* Reading takes only the GPT sector size of the layer settings. */
	fsimg_layer_options_init(&verify->layers);
	verify->layers.gpt_sector_bytes = create->layers.gpt_sector_bytes;
	for (unsigned i = FSIMG_LAYER_SCHEME; i < FSIMG_STACK_SLOTS; i++) {
		if (verify->stack.slots[i].state == FSIMG_SLOT_UNSPECIFIED)
			verify->stack.slots[i].state = FSIMG_SLOT_EMPTY;
	}
	verify->input_fd = fsimg_host_source_open(paths->image.directory,
	    paths->image.kind->file, false);
	if (verify->input_fd == -1)
		return (host_error(error, "cannot open the staged image for verification"));
	if (fsimg_host_file(verify->input_fd, &identity) == -1)
		return (host_error(error, "staged image identity query failed"));
	if (identity.kind != FSIMG_HOST_FILE)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "staged image is not a regular file"));
	return (prepare_spool(verify, error));
}

static bool
publication_cancelled(void *argument)
{
	publication_cancel_t *cancel = argument;

	return (*cancel->interrupted != 0 ||
	    (cancel->deadline != 0 && fsimg_cli_clock() >= cancel->deadline));
}

/* Publish a staged output of length bytes under its final name, without
 * replacement, under the command's limits, its deadline and its interrupt.
 * An unsupported or uncertain publication retains the staged file for
 * inspection; any other failure leaves it for cleanup to remove, which is
 * safe even when linking the final name may have succeeded.
 */
static fsimg_status_t
publish_staged(const fsimg_cli_options_t *options, fsimg_cli_staged_t *staged,
    uint64_t length, const volatile sig_atomic_t *interrupted,
    fsimg_error_t *error)
{
	publication_cancel_t cancel = { .interrupted = interrupted,
	    .deadline = options->request.deadline_milliseconds };
	fsimg_options_t limits = options->request.limits;
	fsimg_ctx_t *ctx;
	fsimg_status_t status;

	limits.scratch_directory_fd = -1;
	limits.cancelled = publication_cancelled;
	limits.cancel_argument = &cancel;
	status = fsimg_ctx_new(&limits, &ctx, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_publish(ctx, staged->directory, staged->kind->file, length,
	    staged->parent, staged->name, NULL, error);
	fsimg_ctx_free(ctx);
	if (status == FSIMG_OK) {
		staged->published = true;
		return (FSIMG_OK);
	}
	staged->published = error->output_state == FSIMG_OUTPUT_PUBLISHED;
	staged->keep_staged = status == FSIMG_EUNSUPPORTED ||
	    error->output_state == FSIMG_OUTPUT_UNCERTAIN;
	if (error->path[0] == '\0')
		fsimg_error_path(error, staged->path);
	return (status);
}

fsimg_status_t
fsimg_cli_publish(const fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    const fsimg_worker_result_t *result, const volatile sig_atomic_t *interrupted,
    fsimg_error_t *error)
{
	return (publish_staged(options, &paths->image, result->image_bytes,
	    interrupted, error));
}

/* The manifest's length is its staged file's own: the coordinator's
 * descriptor names the file the worker wrote, and publication refuses a
 * staged name that no longer holds a file of that length.
 */
fsimg_status_t
fsimg_cli_publish_mtree(const fsimg_cli_options_t *options,
    fsimg_cli_paths_t *paths, const volatile sig_atomic_t *interrupted,
    fsimg_error_t *error)
{
	struct stat st;

	if (fsimg_host_fstat(options->request.manifest_fd, &st) == -1)
		return (host_error(error, "staged manifest stat failed"));
	return (publish_staged(options, &paths->manifest, (uint64_t)st.st_size,
	    interrupted, error));
}

bool
fsimg_cli_published(const fsimg_cli_paths_t *paths)
{
	return (paths->image.published || paths->manifest.published);
}

/*
 * Remove a staged output's file and private directory unless publication
 * retained them, make a published name durable on success, and close the
 * output's descriptors. Returns the first failure's errno, or zero. A
 * private directory without a descriptor, one that failed its check or
 * could not be reopened, is left untouched and reported as retained.
 */
static int
clean_staged(fsimg_cli_staged_t *staged, bool success)
{
	size_t length;
	int failure;

	failure = 0;
	if (staged->created && staged->keep_staged) {
		/* Publication left the staged file for inspection; name it. */
		length = strlen(staged->retained);
		snprintf(staged->retained + length, sizeof(staged->retained) - length,
		    "/%s", staged->kind->file);
	} else if (staged->created && staged->directory != -1) {
		if (unlinkat(staged->directory, staged->kind->file, 0) == -1 &&
		    errno != ENOENT)
			failure = errno;
		if (fsimg_host_remove_directory(staged->parent, staged->directory_name,
		    staged->directory))
			staged->retained[0] = '\0';
		else if (failure == 0)
			failure = errno;
	}
	if (success && staged->parent != -1 && fsync(staged->parent) == -1 &&
	    failure == 0)
		failure = errno;
	if (staged->directory != -1 && close(staged->directory) == -1 &&
	    failure == 0)
		failure = errno;
	if (staged->parent != -1 && close(staged->parent) == -1 && failure == 0)
		failure = errno;
	staged->directory = staged->parent = -1;
	return (failure);
}

/* Scratch uses flat files. Never recurse into a directory left by a failed
 * or compromised worker. A finite cleanup limit bounds coordinator work;
 * anything retained is private and reported, rather than silently ignored.
 */
static int
clean_scratch(int root)
{
	unsigned char buffer[16384];
	const char *name;
	size_t used, cursor, length;
	unsigned entries, interruptions;
	int fd, code, saved;

	fd = openat(root, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd == -1)
		return (errno);
	entries = interruptions = 0;
	saved = 0;
	for (;;) {
		code = fsimg_host_directory_read(fd, buffer, sizeof(buffer), &used);
		if (code == -1 && errno == EINTR && interruptions++ < 1024)
			continue;
		if (code == -1) {
			saved = errno;
			break;
		}
		if (used == 0)
			break;
		interruptions = 0;
		cursor = 0;
		while ((code = fsimg_host_directory_name(buffer, used, &cursor, &name, &length)) > 0) {
			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
				continue;
			if (entries++ == 4096) {
				saved = E2BIG;
				break;
			}
			if (unlinkat(root, name, 0) == -1) {
				saved = errno;
				break;
			}
		}
		if (code == -1)
			saved = errno;
		if (saved != 0)
			break;
	}
	if (close(fd) == -1 && saved == 0)
		saved = errno;
	return (saved);
}

/* Close *fd if held and mark it closed, keeping the first failure. */
static void
close_held(int *fd, int *cleanup)
{
	if (*fd != -1 && close(*fd) == -1 && *cleanup == 0)
		*cleanup = errno;
	*fd = -1;
}

fsimg_status_t
fsimg_cli_cleanup(fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    fsimg_status_t status, fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	int cleanup, failure;
	fsimg_cli_staged_t *staged[] = { &paths->image, &paths->manifest };

	cleanup = 0;
	/* A staged creation's output descriptor is image.directory, lent,
	 * which the image's own cleanup closes.
	 */
	if (request->output_fd != -1 && request->output_fd == paths->image.directory)
		request->output_fd = -1;
	for (unsigned i = 0; i < sizeof(staged) / sizeof(staged[0]); i++) {
		failure = clean_staged(staged[i], status == FSIMG_OK);
		if (cleanup == 0)
			cleanup = failure;
	}
	/* Like a staged output's, a scratch directory without a descriptor is
	 * left untouched and reported.
	 */
	if (paths->scratch_created && request->scratch_fd != -1) {
		failure = clean_scratch(request->scratch_fd);
		if (failure == 0 && !fsimg_host_remove_directory(paths->scratch_parent,
		    paths->scratch_name, request->scratch_fd))
			failure = errno;
		if (failure == 0)
			paths->retained_scratch[0] = '\0';
		else if (cleanup == 0)
			cleanup = failure;
	}
	if (status == FSIMG_OK && paths->destination_parent != -1 &&
	    fsync(paths->destination_parent) == -1 && cleanup == 0)
		cleanup = errno;
	for (fsimg_capability_t c = 0; c < FSIMG_CAPABILITY_COUNT; c++)
		close_held(fsimg_job_capability_field(request, c), &cleanup);
	close_held(&paths->destination_parent, &cleanup);
	close_held(&paths->scratch_parent, &cleanup);
	for (uint32_t i = 0; paths->root_fds != NULL && i < request->root_count; i++) {
		if (paths->root_fds[i] != -1 && close(paths->root_fds[i]) == -1 &&
		    cleanup == 0)
			cleanup = errno;
	}
	free(paths->root_fds);
	free(paths->root_paths);
	paths->root_fds = NULL;
	paths->root_paths = NULL;
	request->root_fds = NULL;
	request->root_paths = NULL;
	request->root_count = 0;
	if (cleanup != 0) {
		if (status == FSIMG_OK)
			status = fsimg_error_set(error, fsimg_cli_published(paths) ?
			    FSIMG_EDURABILITY : FSIMG_EIO,
			    cleanup, FSIMG_OP_IO, "private capability cleanup incomplete");
		error->cleanup_error = cleanup;
	}
	/* A failure's output state, in order: published when any output was
	 * (fsimg_cli_published); else an uncertain publication stays
	 * uncertain; else incomplete for a prepared extraction, or for a
	 * stream once any byte may have reached stdout; else absent for a
	 * creation, a stream before its first byte included.
	 */
	if (status != FSIMG_OK) {
		if (fsimg_cli_published(paths))
			error->output_state = FSIMG_OUTPUT_PUBLISHED;
		else if (error->output_state != FSIMG_OUTPUT_UNCERTAIN) {
			if (paths->extraction_prepared ||
			    ((request->flags & FSIMG_JOB_STDOUT) != 0 &&
			    request->output_budget != NULL &&
			    request->output_budget->stdout_possible))
				error->output_state = FSIMG_OUTPUT_INCOMPLETE;
			else if (request->kind == FSIMG_JOB_CREATE)
				error->output_state = FSIMG_OUTPUT_ABSENT;
		}
	}
	return (status);
}
