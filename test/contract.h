/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_TEST_CONTRACT_H
#define FSIMG_TEST_CONTRACT_H

#include <stdbool.h>
#include <stdint.h>

#include "fsimg.h"

/*
 * Shared apparatus of the driver and layer contract suites
 * (specs/design-driver-contract-tests.md): case bookkeeping and diagnostics,
 * a canaried callback volume with fault injection and access records, a
 * disposable fixture directory, resource baselines, the deterministic
 * payload recipe, and the private-state observations the scenarios need.
 * Nothing here knows a format or a layer.
 */

/*
 * Cases. A case is one (profile, scenario) pair the runner scheduled. Every
 * scheduled case either completes or fails the program with a diagnostic
 * naming the profile, scenario, phase and fault position, so the completion
 * count proves each one ran. A command-line selector "profile/scenario"
 * (either part may be "*") replays a subset; the ordinary run selects all.
 */
typedef struct contract_run {
	const char	*selector;
	uint64_t	 scheduled;
	uint64_t	 completed;
	uint64_t	 not_applicable;
	/* The current case, for diagnostics. */
	const char	*profile;
	const char	*scenario;
	const char	*phase;
	uint64_t	 fault;
} contract_run_t;

extern contract_run_t contract_run;

void contract_init(int, char **);
/* Whether the selector chooses the case; a chosen case is scheduled. */
bool contract_begin(const char *, const char *);
void contract_phase(const char *, uint64_t);
void contract_end(void);
/* A conditional case a declared semantic condition excludes. */
void contract_not_applicable(const char *, const char *, const char *);
/* Report the counts; fails unless every scheduled case completed. */
int contract_finish(const char *);

#if defined(__GNUC__) || defined(__clang__)
#define CONTRACT_NORETURN __attribute__((noreturn))
#else
#define CONTRACT_NORETURN
#endif

void contract_fail(const char *, int, const char *) CONTRACT_NORETURN;
void contract_fail_status(const char *, int, fsimg_status_t, fsimg_status_t,
    const fsimg_error_t *) CONTRACT_NORETURN;

#define CONTRACT_CHECK(condition) do { \
	if (!(condition)) \
		contract_fail(__FILE__, __LINE__, #condition); \
} while (0)

/* Expect an exact status, printing the error record when it differs. */
#define CONTRACT_STATUS(actual, expected, error) do { \
	fsimg_status_t contract_actual_ = (actual); \
	if (contract_actual_ != (expected)) \
		contract_fail_status(__FILE__, __LINE__, contract_actual_, \
		    (expected), (error)); \
} while (0)

/*
 * The canaried volume. One backing buffer holds CONTRACT_GUARD canary bytes,
 * the volume, and CONTRACT_GUARD more canary bytes; scenarios use a public
 * slice of it, so a driver sees only the volume. Every backing callback
 * records its range, and one outside the volume fails the case at once,
 * even for zero bytes. Faults count calls from one and zero disables each.
 */
#define CONTRACT_GUARD	4096u

typedef struct contract_volume {
	unsigned char	*bytes;		/* guard, volume, guard */
	uint64_t	 length;	/* the volume */
	bool		 writable;
	/* Observations. */
	uint64_t	 reads;
	uint64_t	 writes;
	uint64_t	 zeroes;
	uint64_t	 flushes;
	uint64_t	 written_bytes;
	/* Faults. */
	size_t		 max_transfer;	/* positive short transfers */
	bool		 no_zero_range;
	uint64_t	 fail_write;	/* confirms half, then EIO */
	uint64_t	 stall_write;	/* OK with nothing transferred */
	uint64_t	 fail_zero;
	bool		 fail_flush;
	uint64_t	 fail_read;	/* EIO with nothing transferred */
	uint64_t	 eof_read;	/* OK with nothing transferred */
	uint64_t	 confirmed_end;	/* volume offset after the failed write */
	bool		 fault_fired;
} contract_volume_t;

/* Allocate a volume of length bytes, its contents filled with fill. */
void contract_volume_init(contract_volume_t *, uint64_t, unsigned char);
void contract_volume_free(contract_volume_t *);
/* Clear observations and faults, keeping the bytes. */
void contract_volume_reset(contract_volume_t *);
/* The volume as a slice of its canaried backing object; free the slice
 * then the backing object with contract_volume_close().
 */
fsimg_io_t *contract_volume_open(fsimg_ctx_t *, contract_volume_t *, bool,
    fsimg_io_t **);
void contract_volume_close(fsimg_io_t *, fsimg_io_t *);
unsigned char *contract_volume_bytes(contract_volume_t *);
/* Whether both guards still hold their canaries. */
bool contract_volume_guards_intact(const contract_volume_t *);

/* The payload recipe: a deterministic byte of a file at an offset, derived
 * from the file's identity and offset alone.
 */
unsigned char contract_pattern(uint64_t, uint64_t);

/*
 * A disposable fixture directory beneath the working directory, private to
 * one case. Closing it fails the case when anything remains inside, such as
 * a scratch file an operation did not release.
 */
typedef struct contract_dir {
	char		 path[64];
	int		 fd;
} contract_dir_t;

void contract_dir_open(contract_dir_t *);
void contract_dir_close(contract_dir_t *);

/*
 * Resources: a context's live charges are recorded before operation
 * handles exist and compared after they are released. Peaks and cumulative
 * I/O counters are monotonic and not compared.
 */
void contract_baseline(fsimg_ctx_t *, fsimg_stats_t *);
void contract_check_released(fsimg_ctx_t *, const fsimg_stats_t *);

/*
 * Private observations, in one place: whether an armed allocation or
 * scratch fault actually fired, and the writer state that has no public
 * observation (its source handle and failed state).
 */
void contract_arm_alloc(fsimg_ctx_t *, uint64_t);
bool contract_alloc_fired(fsimg_ctx_t *);
void contract_arm_meta(fsimg_ctx_t *, int, uint64_t);
bool contract_meta_fired(fsimg_ctx_t *);
bool contract_writer_source_active(const fsimg_writer_t *);
bool contract_writer_failed(const fsimg_writer_t *);

/* The generous ceiling on any fault sweep's positions. */
#define CONTRACT_SWEEP_MAX	20000u

#endif
