/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cli.h"
#include "host.h"
#include "internal.h"

static volatile sig_atomic_t interrupted;

static void catch_signal(int);
static bool install_signals(void);
static bool hold_standard_descriptors(void);
static void output_failure_state(const fsimg_cli_paths_t *, fsimg_error_t *);
static fsimg_status_t verify_staged(const char *, fsimg_cli_options_t *,
    fsimg_cli_paths_t *, fsimg_cli_display_t *, fsimg_error_t *);

/*
 * Hold each closed standard descriptor with the null device opened the
 * opposite way, so it fails every use as a closed one does, while no
 * descriptor the coordinator opens later (scratch, staging, the spool)
 * can take its number and be handed to a relay as the caller's stream.
 * Descriptors are tried in order, so each open takes the closed number.
 */
static bool
hold_standard_descriptors(void)
{
	for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; fd++) {
		if (fcntl(fd, F_GETFD) != -1 || errno != EBADF)
			continue;
		if (open("/dev/null", fd == STDIN_FILENO ? O_WRONLY : O_RDONLY) != fd)
			return (false);
	}
	return (true);
}

static void
catch_signal(int number)
{
	interrupted = number;
}

static bool
install_signals(void)
{
	const int signals[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };
	struct sigaction action;

	bzero(&action, sizeof(action));
	sigemptyset(&action.sa_mask);
	action.sa_handler = catch_signal;
	for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		if (sigaction(signals[i], &action, NULL) == -1)
			return (false);
	}
	action.sa_handler = SIG_IGN;
	return (sigaction(SIGPIPE, &action, NULL) == 0);
}

static void
output_failure_state(const fsimg_cli_paths_t *paths, fsimg_error_t *error)
{
	if (fsimg_cli_published(paths))
		error->output_state = FSIMG_OUTPUT_PUBLISHED;
	else if (paths->extraction_prepared)
		error->output_state = FSIMG_OUTPUT_INCOMPLETE;
}

/*
 * cW: after the creation worker exits, a second worker under the
 * image-reading profile verifies the staged image before publication. Its
 * events reach the display marked as verification; a failure leaves the
 * staged image unpublished, and cleanup discards it.
 */
static fsimg_status_t
verify_staged(const char *executable, fsimg_cli_options_t *options,
    fsimg_cli_paths_t *paths, fsimg_cli_display_t *display, fsimg_error_t *error)
{
	fsimg_worker_request_t verify;
	fsimg_worker_result_t result = { .confinement = FSIMG_CONFINEMENT_UNKNOWN };
	fsimg_status_t status;

	status = fsimg_cli_verification_request(options, paths, &verify, error);
	if (status == FSIMG_OK) {
		display->verifying_staged = true;
		status = fsimg_worker_run(executable, &verify, fsimg_cli_event, display,
		    STDOUT_FILENO, &interrupted, &result, error);
		display->verifying_staged = false;
	}
	if (verify.input_fd != -1)
		close(verify.input_fd);
	if (verify.spool_fd != -1)
		close(verify.spool_fd);
	/* Only a verdict on the image is a failed verification; cancellation,
	 * a deadline, a resource limit or unavailable confinement leaves the
	 * image unjudged, and the error itself says so.
	 */
	paths->verification_failed = status == FSIMG_ECORRUPT;
	return (status);
}

int
main(int argc, char **argv)
{
	fsimg_cli_options_t options;
	fsimg_cli_paths_t paths;
	fsimg_cli_display_t display;
	fsimg_worker_result_t result = { .confinement = FSIMG_CONFINEMENT_UNKNOWN };
	fsimg_output_budget_t budget;
	fsimg_error_t error, output_error;
	fsimg_status_t status, output_status;
	char executable[PATH_MAX];
	uint64_t now, reporting_deadline;
	bool invalid_syntax;

	if (argc == 2 && strcmp(argv[1], "--internal-worker") == 0)
		return (fsimg_worker_main());
	if (argc == 2 && strcmp(argv[1], "--internal-output") == 0)
		return (fsimg_output_main());
	if (argc == 2 && strcmp(argv[1], "--internal-input") == 0)
		return (fsimg_input_main());
	if (!hold_standard_descriptors())
		return (1);
	/* Without a reliable executable identity or signal setup there is no
	 * supervised output path. Refuse startup instead of writing unsupervised.
	 */
	if (fsimg_host_executable(executable, sizeof(executable)) == -1 || !install_signals())
		return (1);
	fsimg_cli_paths_init(&paths);
	status = fsimg_cli_parse(argc, argv, &options, &error);
	bzero(&budget, sizeof(budget));
	options.request.output_budget = &budget;
	invalid_syntax = status != FSIMG_OK && options.syntax_error;
	if (status == FSIMG_OK && options.request.seconds != 0) {
		now = fsimg_cli_clock();
		if (now == 0)
			status = fsimg_error_set(&error, FSIMG_EIO, errno,
			    FSIMG_OP_CONTEXT, "monotonic clock query failed");
		else if (options.request.seconds > (UINT64_MAX - now) / 1000) {
			status = fsimg_error_set(&error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "deadline exceeds monotonic clock range");
			invalid_syntax = true;
		} else
			options.request.deadline_milliseconds = now + options.request.seconds * 1000;
	}
	fsimg_cli_display_init(&display, &options, &result, &paths);
	display.syntax = invalid_syntax;
	if (status == FSIMG_OK && options.command == FSIMG_CLI_PROCESS) {
		status = fsimg_cli_prepare(&options, &paths, &error);
		if (status == FSIMG_OK)
			status = fsimg_worker_run(executable, &options.request,
			    fsimg_cli_event, &display, STDOUT_FILENO, &interrupted, &result, &error);
		/* The raw intermediate and the spool are spent once their worker
		 * exits; closing them frees their blocks before verification and
		 * publication.
		 */
		if (options.request.raw_fd != -1) {
			close(options.request.raw_fd);
			options.request.raw_fd = -1;
		}
		if (options.request.spool_fd != -1) {
			close(options.request.spool_fd);
			options.request.spool_fd = -1;
		}
		if (status == FSIMG_OK && options.verify_staged)
			status = verify_staged(executable, &options, &paths, &display, &error);
		/* A stream has no publication; its bytes are already delivered. */
		if (status == FSIMG_OK && fsimg_job_needs_staging(&options.request))
			status = fsimg_cli_publish(&options, &paths, &result, &interrupted, &error);
		if (status == FSIMG_OK && options.request.manifest_fd != -1)
			status = fsimg_cli_publish_mtree(&options, &paths, &interrupted,
			    &error);
		status = fsimg_cli_cleanup(&options, &paths, status, &error);
	}
	if (status != FSIMG_OK)
		display.failure = &error;
	reporting_deadline = options.request.deadline_milliseconds;
	if (status != FSIMG_OK) {
		/* Processing has stopped. Give a failed operation a bounded diagnostic
		 * opportunity, including when its deadline or first signal caused it.
		 * A subsequent signal can still interrupt this final report.
		 */
		interrupted = 0;
		reporting_deadline = fsimg_cli_clock() + 2000;
	}
	output_status = fsimg_output_run(executable, fsimg_cli_final, &display,
	    STDOUT_FILENO, reporting_deadline, &interrupted, &budget, &output_error);
	if (output_status != FSIMG_OK) {
		if (status == FSIMG_OK) {
			status = output_status;
			error = output_error;
			output_failure_state(&paths, &error);
		}
		/* A broken stdout cannot carry an error record. One bounded stderr
		 * attempt uses the same relay with no stdout authority.
		 */
		options.json = false;
		display.failure = &error;
		display.entry_open = false;
		display.utf8_used = display.utf8_needed = 0;
		display.phase = FSIMG_CLI_OUTPUT_RECORDS;
		interrupted = 0;
		fsimg_output_run(executable, fsimg_cli_final, &display, -1,
		    fsimg_cli_clock() + 2000, &interrupted, &budget, &output_error);
	}
	fsimg_cli_release(&options);
	return (status == FSIMG_OK ? 0 : invalid_syntax ? 2 : 1);
}
