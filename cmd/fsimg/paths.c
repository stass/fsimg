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

static fsimg_status_t host_error(fsimg_error_t *, const char *);
static fsimg_status_t path_error(fsimg_error_t *, const char *, const char *);
static fsimg_status_t split_output(const char *, char *, char *, fsimg_error_t *);
static fsimg_status_t private_directory(int, const char *, char *, int *, bool *,
    fsimg_error_t *);
static fsimg_status_t reject_ancestor(int, int, fsimg_error_t *);
static fsimg_status_t prepare_scratch(fsimg_cli_options_t *, fsimg_cli_paths_t *,
    fsimg_error_t *);
static fsimg_status_t open_input(fsimg_cli_options_t *, fsimg_error_t *);
static fsimg_status_t open_contents(int, const fsimg_cli_operand_t *, int,
    int *, fsimg_error_t *);
static fsimg_status_t hold_descriptor(int, int, const char *, const char *, int *,
    fsimg_error_t *);
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
static fsimg_status_t privacy_error(fsimg_error_t *, fsimg_host_privacy_t, bool,
    const char *);
static bool remove_fresh_directory(int, const char *, int);
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
	int count;

	status = host_error(error, message);
	if (error != NULL) {
		count = snprintf(error->path, sizeof(error->path), "%s", path);
		error->path_truncated = count < 0 || (size_t)count >= sizeof(error->path);
	}
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

static fsimg_status_t
private_directory(int parent, const char *role, char *name, int *out,
    bool *created, fsimg_error_t *error)
{
	uint64_t random[2];

	for (unsigned attempt = 0; attempt < 128; attempt++) {
		if (fsimg_host_random(random, sizeof(random)) == -1)
			return (host_error(error, "private directory randomness failed"));
		snprintf(name, 96, ".fsimg-%s-%016llx%016llx", role,
		    (unsigned long long)random[0], (unsigned long long)random[1]);
		if (mkdirat(parent, name, 0700) == 0) {
			*created = true;
			*out = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (*out == -1)
				return (host_error(error, "private directory open failed"));
			return (FSIMG_OK);
		}
		if (errno != EEXIST)
			return (host_error(error, "private directory creation failed"));
	}
	return (fsimg_error_set(error, FSIMG_EEXISTS, EEXIST,
	    FSIMG_OP_IO, "private directory name attempts exhausted"));
}

/* Compare opened identities while walking toward the host root. No pathname
 * supplied by a worker participates. Both directions are checked because
 * scratch deletion authority must never contain the output tree.
 */
static fsimg_status_t
reject_ancestor(int descendant, int ancestor, fsimg_error_t *error)
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
			    FSIMG_OP_IO, "scratch and output trees overlap");
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
		    FSIMG_OP_IO, "scratch parent leaves no room for a private pathname"));
	paths->scratch_parent = open(options->scratch, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (paths->scratch_parent == -1)
		return (path_error(error, "cannot open scratch directory", options->scratch));
	status = private_directory(paths->scratch_parent, "scratch", paths->scratch_name,
	    &request->scratch_fd, &paths->scratch_created, error);
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
	if (request->input_fd == -1)
		return (path_error(error, "cannot open image", options->image));
	if (fsimg_host_file(request->input_fd, &identity) == -1)
		return (host_error(error, "image identity query failed"));
	if (identity.kind != FSIMG_HOST_FILE)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "input image must be a seekable regular file"));
	return (FSIMG_OK);
}

/*
 * The no-follow scanned-object open of a command-line path. O_NOFOLLOW
 * covers only the final component, and a path ending in "/" or "/." makes
 * the kernel resolve a symbolic link before that component, so trailing
 * slashes and "." components are removed first: "link/." then names the
 * link itself and fails. A path left with no component means the base
 * directory itself (".") or the root ("/"). A refused link fails with ELOOP
 * on every host: a no-follow directory open of one reports ENOTDIR on
 * macOS, so an lstat of the same name, used only to choose the errno,
 * tells the two apart.
 */
static int
open_nofollow(int base, const char *path, bool directory)
{
	char trimmed[PATH_MAX];
	struct stat st;
	size_t length;
	int fd;
	bool trimmed_slash;

	length = strlen(path);
	if (length >= sizeof(trimmed)) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	bcopy(path, trimmed, length + 1);
	/* A trailing slash names a directory, so it stays a requirement. */
	trimmed_slash = false;
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
		trimmed_slash = true;
	}
	if (trimmed_slash)
		directory = true;
	if (length == 0)
		return (fsimg_host_source_open(base, ".", directory));
	fd = fsimg_host_source_open(base, trimmed, directory);
	if (fd == -1 && errno == ENOTDIR &&
	    fstatat(base, trimmed, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode))
		errno = ELOOP;
	return (fd);
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
			return (path_error(error, operand->manifest ?
			    "manifest is a symbolic link" : "operand is a symbolic link",
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
		errno = 0;
		return (path_error(error, operand->manifest ?
		    "manifest is not a regular file" :
		    "operand is not a regular file or directory", operand->host_path));
	}
	return (hold_descriptor(fd, first_free, operand->manifest ?
	    "cannot hold manifest" : "cannot hold operand", operand->host_path,
	    out, error));
}

/*
 * A manifest's candidate content directory: the -C directory in effect, or
 * the working directory, opened as an operand of "." is, beneath which the
 * worker reads a file's bytes only when the manifest's entry needs them.
 */
static fsimg_status_t
open_contents(int directory, const fsimg_cli_operand_t *operand,
    int first_free, int *out, fsimg_error_t *error)
{
	const char *name;
	int fd;

	name = operand->directory == NULL ? "." : operand->directory;
	fd = fsimg_host_source_open(directory, ".", true);
	if (fd == -1)
		return (path_error(error, "cannot open the manifest's content directory",
		    name));
	return (hold_descriptor(fd, first_free,
	    "cannot hold the manifest's content directory", name, out, error));
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
 * given. A host operand is one root descriptor; a manifest is two
 * consecutive ones, the manifest and its candidate content directory, each
 * with the path FSIMG_JOB_MANIFEST, which the worker adopts together as one
 * source root. The worker holds one descriptor per root descriptor
 * besides its own: the staging directory of a create job, scratch unless
 * it is disabled, and two while scanning, a directory and the entry read
 * from it; the staged image is opened after scanning, in one of those two.
 * The operands must leave room for those within the fds limit, so an
 * oversized command line fails here, before the worker starts.
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
		count += options->operands[i].manifest ? 2 : 1;
	needed = (uint64_t)count + 2 + (request->kind == FSIMG_JOB_CREATE ? 1 : 0) +
	    (request->scratch_fd != -1 ? 1 : 0);
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
				status = path_error(error, "cannot open -C directory",
				    operand->directory);
				break;
			}
		}
		/* Each slot's path is set even when its open fails; the loop
		 * then ends, and cleanup reads only the descriptors.
		 */
		status = open_operand(directory, operand, first_free,
		    &paths->root_fds[slot], error);
		if (operand->manifest) {
			paths->root_paths[slot] = FSIMG_JOB_MANIFEST;
			paths->root_paths[slot + 1] = FSIMG_JOB_MANIFEST;
			if (status == FSIMG_OK)
				status = open_contents(directory, operand,
				    first_free, &paths->root_fds[slot + 1], error);
			slot += 2;
		} else {
			paths->root_paths[slot] = operand->image_path;
			slot++;
		}
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

/* A fresh private staging directory beside the final name, which must not
 * exist yet. The worker creates the staged image inside it only after
 * planning; only the coordinator publishes it.
 */
static fsimg_status_t
prepare_staging(fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	struct stat st;
	char parent_path[PATH_MAX];
	fsimg_status_t status;
	int count;

	status = split_output(options->image, parent_path, paths->basename, error);
	if (status != FSIMG_OK)
		return (status);
	if (strlen(parent_path) + sizeof(paths->staging_name) + 7 >= PATH_MAX)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_IO, "output parent leaves no room for a private pathname"));
	paths->parent = open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (paths->parent == -1)
		return (path_error(error, "cannot open output directory", parent_path));
	if (fstatat(paths->parent, paths->basename, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		errno = EEXIST;
		return (path_error(error, "cannot create output", options->image));
	}
	if (errno != ENOENT)
		return (path_error(error, "cannot inspect output", options->image));
	status = private_directory(paths->parent, "output", paths->staging_name,
	    &request->output_fd, &paths->staging_created, error);
	if (!paths->staging_created)
		return (status);
	/* A post-create open failure still needs a retained staging path. */
	count = snprintf(paths->retained_output, sizeof(paths->retained_output),
	    "%s/%s/image", parent_path, paths->staging_name);
	if (status != FSIMG_OK)
		return (status);
	if (count < 0 || (size_t)count >= sizeof(paths->retained_output))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_IO, "private output pathname exceeds host path bound"));
	return (FSIMG_OK);
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

/* A destination that fails the privacy rule. A fresh directory carrying an
 * ACL got it from the -C parent's inheritable entries, so that error names
 * the parent; every other verdict names the destination itself.
 */
static fsimg_status_t
privacy_error(fsimg_error_t *error, fsimg_host_privacy_t verdict, bool fresh,
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
		message = fresh ?
		    "-C directory gives new directories a non-trivial access control list" :
		    "extraction destination carries a non-trivial access control list";
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

/* Remove the directory just created under name when it is still that
 * object, which the opened descriptor identifies, still owned by this user,
 * so a directory someone else substituted for it is never removed, and
 * still empty, which rmdir itself requires. Anything else stays and is
 * reported.
 */
static bool
remove_fresh_directory(int parent, const char *name, int fd)
{
	struct stat opened, named;

	if (fstat(fd, &opened) == -1 ||
	    fstatat(parent, name, &named, AT_SYMLINK_NOFOLLOW) == -1)
		return (false);
	if (opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
	    opened.st_uid != geteuid())
		return (false);
	return (unlinkat(parent, name, AT_REMOVEDIR) == 0);
}

/*
 * The extraction destination, proven private before the worker starts.
 * By default it is a fresh directory named after the image stem: one
 * mkdirat with mode 0700 beneath the opened -C directory, so an existing
 * name of any kind fails, then a no-follow reopen. With -E it is the -C
 * directory itself, opened no-follow. Either way the opened descriptor
 * alone must pass the privacy rule, and the worker receives only it.
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
			return (path_error(error, errno == ELOOP ?
			    "-C directory is a symbolic link" : "cannot open -C directory",
			    parent_path));
		if (fsimg_host_private_directory(request->output_fd, &verdict) == -1)
			return (path_error(error, "cannot inspect -C directory", parent_path));
		if (verdict != FSIMG_HOST_PRIVATE)
			return (privacy_error(error, verdict, false, parent_path));
		paths->extraction_prepared = true;
		snprintf(paths->retained_output, sizeof(paths->retained_output), "%s",
		    parent_path);
		return (FSIMG_OK);
	}

	paths->parent = open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (paths->parent == -1)
		return (path_error(error, "cannot open -C directory", parent_path));
	count = snprintf(paths->retained_output, sizeof(paths->retained_output),
	    "%s/%s", parent_path, options->stem);
	if (count < 0 || (size_t)count >= sizeof(paths->retained_output))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_IO, "extraction pathname exceeds host path bound"));
	if (mkdirat(paths->parent, options->stem, 0700) == -1) {
		status = path_error(error, "cannot create extraction directory",
		    paths->retained_output);
		paths->retained_output[0] = '\0';
		return (status);
	}
	paths->extraction_prepared = true;
	request->output_fd = openat(paths->parent, options->stem,
	    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (request->output_fd == -1)
		return (path_error(error, "cannot open extraction directory",
		    paths->retained_output));
	if (fsimg_host_private_directory(request->output_fd, &verdict) == -1)
		return (path_error(error, "cannot inspect extraction directory",
		    paths->retained_output));
	if (verdict == FSIMG_HOST_PRIVATE)
		return (FSIMG_OK);
	status = privacy_error(error, verdict, true,
	    verdict == FSIMG_HOST_EXTENDED_ACL ? parent_path : paths->retained_output);
	if (remove_fresh_directory(paths->parent, options->stem, request->output_fd)) {
		paths->extraction_prepared = false;
		paths->retained_output[0] = '\0';
	}
	return (status);
}

fsimg_status_t
fsimg_cli_prepare(fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	fsimg_status_t status;

	bzero(paths, sizeof(*paths));
	paths->parent = paths->scratch_parent = -1;
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
		status = open_roots(options, paths, error);
		if (status == FSIMG_OK)
			status = generate_identifiers(options, error);
		if (status == FSIMG_OK && request->kind == FSIMG_JOB_CREATE &&
		    (request->flags & FSIMG_JOB_STDOUT) == 0)
			status = prepare_staging(options, paths, error);
		if (status == FSIMG_OK)
			status = prepare_raw(options, error);
		break;
	case FSIMG_JOB_EXTRACT:
		status = open_input(options, error);
		if (status == FSIMG_OK)
			status = prepare_destination(options, paths, error);
		break;
	default:
		status = open_input(options, error);
		break;
	}
	if (status != FSIMG_OK)
		return (status);

	if (request->output_fd != -1 && request->scratch_fd != -1) {
		status = reject_ancestor(request->output_fd, request->scratch_fd, error);
		if (status != FSIMG_OK)
			return (status);
		status = reject_ancestor(request->scratch_fd, request->output_fd, error);
	}
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
    fsimg_worker_request_t *verify, fsimg_error_t *error)
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
	verify->offset = verify->length = 0;
	verify->selector = FSIMG_SELECT_AUTOMATIC;
	/* Reading takes only the GPT sector size of the layer settings. */
	fsimg_layer_options_init(&verify->layers);
	verify->layers.gpt_sector_bytes = create->layers.gpt_sector_bytes;
	for (unsigned i = FSIMG_LAYER_SCHEME; i < FSIMG_STACK_SLOTS; i++) {
		if (verify->stack.slots[i].state == FSIMG_SLOT_UNSPECIFIED)
			verify->stack.slots[i].state = FSIMG_SLOT_EMPTY;
	}
	verify->input_fd = fsimg_host_source_open(create->output_fd, "image", false);
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

fsimg_status_t
fsimg_cli_publish(const fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    const fsimg_worker_result_t *result, const volatile sig_atomic_t *interrupted,
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
	status = fsimg_publish(ctx, options->request.output_fd, "image", result->image_bytes,
	    paths->parent, paths->basename, NULL, error);
	paths->published = status == FSIMG_OK || error->output_state == FSIMG_OUTPUT_PUBLISHED;
	paths->retain_staging = status == FSIMG_EUNSUPPORTED ||
	    error->output_state == FSIMG_OUTPUT_UNCERTAIN;
	fsimg_ctx_free(ctx);
	return (status);
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

fsimg_status_t
fsimg_cli_cleanup(fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    fsimg_status_t status, fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	char *last_component;
	int cleanup, failure;
	int *descriptors[] = { &request->input_fd, &request->output_fd,
	    &request->scratch_fd, &request->raw_fd, &request->spool_fd,
	    &paths->parent,
	    &paths->scratch_parent };

	cleanup = 0;
	if (paths->staging_created && !paths->retain_staging) {
		/* Cleanup can remove the image yet retain other directory entries.
		 * Report the private directory, not an image that may no longer exist.
		 */
		last_component = strrchr(paths->retained_output, '/');
		if (last_component != NULL)
			*last_component = '\0';
		if (request->output_fd != -1 &&
		    unlinkat(request->output_fd, "image", 0) == -1 && errno != ENOENT)
			cleanup = errno;
		if (unlinkat(paths->parent, paths->staging_name, AT_REMOVEDIR) == -1) {
			if (cleanup == 0)
				cleanup = errno;
		} else
			paths->retained_output[0] = '\0';
	}
	if (paths->scratch_created) {
		failure = request->scratch_fd == -1 ? 0 : clean_scratch(request->scratch_fd);
		if (failure == 0 &&
		    unlinkat(paths->scratch_parent, paths->scratch_name, AT_REMOVEDIR) == -1)
			failure = errno;
		if (failure == 0)
			paths->retained_scratch[0] = '\0';
		else if (cleanup == 0)
			cleanup = failure;
	}
	if (status == FSIMG_OK && paths->parent != -1 && fsync(paths->parent) == -1 &&
	    cleanup == 0)
		cleanup = errno;
	for (unsigned i = 0; i < sizeof(descriptors) / sizeof(descriptors[0]); i++) {
		if (*descriptors[i] != -1 && close(*descriptors[i]) == -1 && cleanup == 0)
			cleanup = errno;
		*descriptors[i] = -1;
	}
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
			status = fsimg_error_set(error, paths->published ? FSIMG_EDURABILITY : FSIMG_EIO,
			    cleanup, FSIMG_OP_IO, "private capability cleanup incomplete");
		error->cleanup_error = cleanup;
	}
	/* A stream has no publication: once any byte may have reached stdout,
	 * the consumer holds an incomplete image; before that, none.
	 */
	if (status != FSIMG_OK) {
		if (paths->published)
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
