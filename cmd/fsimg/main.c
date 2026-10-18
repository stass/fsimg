/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <errno.h>
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
static void output_failure_state(const fsimg_cli_paths_t *, fsimg_error_t *);

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
	if (paths->published)
		error->output_state = FSIMG_OUTPUT_PUBLISHED;
	else if (paths->extraction_created)
		error->output_state = FSIMG_OUTPUT_INCOMPLETE;
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
	/* Without a reliable executable identity or signal setup there is no
	 * supervised output path. Refuse startup instead of writing unsupervised.
	 */
	if (fsimg_host_executable(executable, sizeof(executable)) == -1 || !install_signals())
		return (1);
	bzero(&paths, sizeof(paths));
	paths.parent = paths.scratch_parent = -1;
	status = fsimg_cli_parse(argc, argv, &options, &error);
	fsimg_output_budget_init(&budget, &options.request.limits);
	options.request.output_budget = &budget;
	invalid_syntax = status != FSIMG_OK;
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
		if (status == FSIMG_OK && options.request.kind == FSIMG_JOB_CREATE)
			status = fsimg_cli_publish(&options, &paths, &result, &interrupted, &error);
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
		budget.reporting = true;
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
		budget.reporting = true;
		fsimg_output_run(executable, fsimg_cli_final, &display, -1,
		    fsimg_cli_clock() + 2000, &interrupted, &budget, &output_error);
	}
	return (status == FSIMG_OK ? 0 : invalid_syntax ? 2 : 1);
}
