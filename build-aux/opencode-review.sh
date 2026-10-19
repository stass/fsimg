#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
#
# Run the required external OpenCode review, confined and read-only, and fail
# rather than hang or accept a partial answer.
#
#   build-aux/opencode-review.sh OUTPUT PROMPT-FILE
#
# Confinement follows docs/build.md: the tracked files of the working tree
# are copied into a fresh review directory without the checkout's Git
# metadata, local configuration or build products. The uncommitted change is
# CHANGE.diff (git diff HEAD; mark new files with git add -N first) and the
# prompt is REVIEW.md. Tracked symlinks are not copied; a changed link
# appears only in CHANGE.diff.
#
# OpenCode runs there with a scrubbed environment: plugins and sharing off,
# Claude Code instruction files off, and a fresh private home and
# configuration, data, state and cache directories holding only a copy of
# its credentials. Proxy and CA settings are passed through when set. Every
# permission is denied except read, glob and grep. Before any model request
# the script asks OpenCode for the agent's resolved tools and permissions,
# and refuses to run unless exactly those tools are enabled and every rule
# after the final deny-all keeps access inside the review directory.
#
# An attempt is killed with its whole process group when no event arrives
# within the startup limit, when events stop for the stall limit, or at the
# attempt deadline, and is retried up to the attempt count. Limits and
# signals are acted on at the next poll, and a stopped attempt gets a grace
# period before KILL, so teardown can lag by about 15 seconds. A review is
# accepted only when the final step stops normally without tool calls and
# its text has a body followed by the line END OF REVIEW. Only an accepted
# review is written to OUTPUT; events and diagnostics of attempt N are kept
# in OUTPUT.N.jsonl and OUTPUT.N.stderr.
# Exit status: 0 accepted, 1 every attempt failed, 2 usage or setup error.

set -u
# Reviews, event logs and the credential copy are private to the caller.
umask 077

model=zai-coding-plan/glm-5.3
variant=max
marker='END OF REVIEW'
config='{"share":"disabled","permission":{"*":"deny","read":"allow","glob":"allow","grep":"allow","external_directory":"deny"}}'
attempts=${OPENCODE_REVIEW_ATTEMPTS:-3}
startup_limit=${OPENCODE_REVIEW_STARTUP:-300}
stall_limit=${OPENCODE_REVIEW_STALL:-1200}
deadline=${OPENCODE_REVIEW_DEADLINE:-5400}
poll=10
grace=5
credentials=${XDG_DATA_HOME:-$HOME/.local/share}/opencode/auth.json

usage() {
	echo "usage: $0 OUTPUT PROMPT-FILE" >&2
	exit 2
}

if test $# -ne 2 || test ! -f "$2" || test ! -r "$2"; then
	usage
fi
# Six digits keep every limit inside shell arithmetic.
for value in "$attempts" "$startup_limit" "$stall_limit" "$deadline"; do
	case $value in
	''|*[!0-9]*|0|00*|???????*)
		echo "$0: OPENCODE_REVIEW_* limits must be 1 to 999999" >&2
		exit 2
		;;
	esac
done
for tool in git opencode python3; do
	if ! command -v "$tool" > /dev/null; then
		echo "$0: $tool is required" >&2
		exit 2
	fi
done
if test ! -r "$credentials"; then
	echo "$0: no OpenCode credentials at $credentials" >&2
	exit 2
fi
top=$(git rev-parse --show-toplevel) || exit 2
top=$(cd "$top" && pwd -P) || exit 2
prompt_file=$(cd "$(dirname "$2")" && pwd -P)/$(basename "$2") || exit 2
output_directory=$(cd "$(dirname "$1")" && pwd -P) || exit 2
output=$output_directory/$(basename "$1")
# OUTPUT and its side files are replaced, so keep them out of the checkout,
# whose file names may be case-insensitive, and away from the prompt.
case $output_directory/ in
"$top"/*)
	echo "$0: OUTPUT must be outside the checkout: $output" >&2
	exit 2
	;;
esac
if test -e "$output" || test -L "$output"; then
	echo "$0: OUTPUT already exists: $output" >&2
	exit 2
fi
if test ! -w "$output_directory"; then
	echo "$0: OUTPUT directory is not writable: $output_directory" >&2
	exit 2
fi
rm -f "$output.tmp" "$output".[0-9]*.jsonl "$output".[0-9]*.stderr

# Stop an attempt: TERM its process group, allow a grace period, then KILL
# whatever remains. The group includes shells and helpers opencode started;
# the leader is also signalled directly in case setsid has not run yet.
# "kill -s SIG -- -PGID" is the form sh, dash and bash all accept.
stop_review() {
	test -n "$pid" || return 0
	kill -s TERM -- "-$pid" 2>/dev/null
	kill -s TERM "$pid" 2>/dev/null
	sleep "$grace"
	kill -s KILL -- "-$pid" 2>/dev/null
	kill -s KILL "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	pid=
}

cleanup() {
	stop_review
	test -z "$review" || rm -rf "$review"
}

pid=
review=
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
review=$(mktemp -d "${TMPDIR:-/tmp}/fsimg-review-XXXXXX") || exit 2
review=$(cd "$review" && pwd -P) || exit 2
# The copy is its own Git work tree, so OpenCode's search for project
# instructions and configuration stops there and never reaches files other
# users can plant in a shared temporary directory.
mkdir -p "$review/home" "$review/config" "$review/data/opencode" \
    "$review/state" "$review/cache" "$review/tree" || exit 2
git init -q "$review/tree" || exit 2
(umask 077 && cp "$credentials" "$review/data/opencode/auth.json") || exit 2

# Run a command in the review tree with only the environment the review
# needs. The private HOME keeps OpenCode from loading ~/.opencode. Call it as
# the last command of a subshell: it execs, so the subshell's pid becomes the
# command's.
confined() {
	cd "$review/tree" || exit 1
	for name in HTTPS_PROXY HTTP_PROXY NO_PROXY https_proxy http_proxy \
	    no_proxy SSL_CERT_FILE NODE_EXTRA_CA_CERTS; do
		if eval "test -n \"\${$name+set}\""; then
			eval "set -- \"$name=\$$name\" \"\$@\""
		fi
	done
	exec env -i PATH="$PATH" HOME="$review/home" TMPDIR="${TMPDIR:-/tmp}" \
	    TERM="${TERM:-dumb}" OPENCODE_CONFIG_CONTENT="$config" \
	    OPENCODE_DISABLE_CLAUDE_CODE=1 XDG_CONFIG_HOME="$review/config" \
	    XDG_DATA_HOME="$review/data" XDG_STATE_HOME="$review/state" \
	    XDG_CACHE_HOME="$review/cache" "$@"
}

# Copy the tracked files as they are in the working tree, so the review sees
# the change applied, and record the change itself. Each path is opened one
# component at a time without following symlinks, and only a regular file is
# copied, so a path swapped for a link cannot pull in another file.
(cd "$top" && git ls-files -z) > "$review/files" || exit 2
(cd "$top" && python3 -I - "$review/files" "$review/tree" <<'EOF') || exit 2
import errno, os, stat, sys

listing, destination = sys.argv[1:3]
names = open(listing, 'rb').read().split(b'\0')
if b'REVIEW.md' in names or b'CHANGE.diff' in names:
	sys.exit('opencode-review: the checkout tracks REVIEW.md or CHANGE.diff')
for name in names:
	if not name:
		continue
	parts = name.split(b'/')
	# fd always holds the one open descriptor: a failed open leaves the
	# parent in it, so the handler below closes exactly that.
	fd = os.open('.', os.O_RDONLY | os.O_DIRECTORY)
	try:
		for part in parts[:-1]:
			parent = fd
			fd = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
			    dir_fd=parent)
			os.close(parent)
		parent = fd
		fd = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
		    dir_fd=parent)
		os.close(parent)
	except OSError as failure:
		os.close(fd)
		# A link (ELOOP, or EMLINK on FreeBSD), a path through a link or
		# a deleted file is left to CHANGE.diff; anything else is an error.
		if failure.errno in (errno.ELOOP, errno.EMLINK, errno.ENOTDIR,
		    errno.ENOENT):
			continue
		sys.exit('opencode-review: cannot copy %s: %s' %
		    (os.fsdecode(name), failure.strerror))
	with os.fdopen(fd, 'rb') as source:
		if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
			continue
		target = os.path.join(os.fsencode(destination), name)
		os.makedirs(os.path.dirname(target), exist_ok=True)
		with open(target, 'wb') as copy:
			copy.write(source.read())
EOF
(cd "$top" && git --no-pager diff --no-color --no-ext-diff --no-textconv \
    HEAD) > "$review/tree/CHANGE.diff" || exit 2

prompt=$review/tree/REVIEW.md
{
	echo "Completion rule: when the review is complete, end your final message"
	echo "with a line containing exactly: $marker"
	echo "Never write that line before the review is complete."
	echo
	echo "This directory is a copy of the repository's tracked files with the"
	echo "change applied. The change under review is CHANGE.diff (git diff HEAD)."
	echo "Only reading tools are available; do not attempt to run commands."
	echo "Read CLAUDE.md first: it holds the project rules the review applies."
	echo
} > "$prompt" || exit 2
cat "$prompt_file" >> "$prompt" || exit 2
{
	echo
	echo "Completion rule: end your final message with a line containing"
	echo "exactly: $marker"
} >> "$prompt" || exit 2

# Run "$@" under confined() as its own process group and leader, so $! is
# both its pid and group id.
launch() {
	confined python3 -I -c '
import os, sys
os.setsid()
os.execvp(sys.argv[1], sys.argv[1:])
' "$@"
}

# Check the agent configuration OpenCode resolves in that environment,
# without a model request, and give up after the startup limit. For each
# permission the last matching rule wins, so every rule after the final
# deny-all must stay inside the review: allowing read, glob or grep, denying
# outside access, or allowing OpenCode's own tool output in the private data
# directory.
(launch opencode debug agent plan --pure) < /dev/null \
    > "$review/agent.json" 2> "$review/agent.stderr" &
pid=$!
waited=0
while kill -0 "$pid" 2>/dev/null && test "$waited" -lt "$startup_limit"; do
	sleep 1
	waited=$((waited + 1))
done
if kill -0 "$pid" 2>/dev/null; then
	stop_review
	echo "$0: OpenCode did not resolve its configuration in ${startup_limit}s" >&2
	exit 2
fi
# The leader exited; helpers left in its group must not outlive it.
kill -s KILL -- "-$pid" 2>/dev/null
wait "$pid"
status=$?
pid=
if test "$status" -ne 0; then
	cat "$review/agent.stderr" >&2
	echo "$0: cannot resolve the OpenCode agent configuration" >&2
	exit 2
fi
python3 -I - "$review/agent.json" "$review" <<'EOF' || exit 2
import json, sys

agent = json.load(open(sys.argv[1], encoding='utf-8'))
tool_output = sys.argv[2] + '/data/opencode/tool-output/*'
tools = agent.get('tools') or {}
enabled = sorted(name for name, on in tools.items() if on is True)
if enabled != ['glob', 'grep', 'read']:
	sys.exit('opencode-review: unexpected enabled tools %r' % (enabled,))
rules = agent.get('permission') or []
final = None
for index, rule in enumerate(rules):
	if rule == {'permission': '*', 'pattern': '*', 'action': 'deny'}:
		final = index
if final is None:
	sys.exit('opencode-review: no deny-all permission rule')
for rule in rules[final + 1:]:
	kind, pattern, action = (rule.get(key) for key in
	    ('permission', 'pattern', 'action'))
	if kind in ('read', 'glob', 'grep') and pattern == '*' and \
	    action == 'allow':
		continue
	if kind == 'external_directory' and pattern == '*' and action == 'deny':
		continue
	if kind == 'external_directory' and pattern == tool_output and \
	    action == 'allow':
		continue
	sys.exit('opencode-review: permission rule escapes the review: %r' %
	    (rule,))
EOF

# Print the size of an event file in bytes.
event_bytes() {
	if test -s "$1"; then
		wc -c < "$1" | tr -d ' '
	else
		echo 0
	fi
}

# Accept the review in event file $1: the final step must stop normally,
# carry the last text and no tool calls, and the text must have a body and
# end with the marker line. The accepted text is written to OUTPUT. Exit
# status 1 rejects the review; 3 means OUTPUT could not be written.
accept_review() {
	python3 -I - "$1" "$output" "$marker" <<'EOF'
import json, os, sys

events, output, marker = sys.argv[1:4]
texts = {}
tool_messages = set()
last_text = None
last_step = None
last_reason = None
for line in open(events, encoding='utf-8', errors='replace'):
	try:
		event = json.loads(line)
	except ValueError:
		continue
	if not isinstance(event, dict):
		continue
	part = event.get('part')
	if not isinstance(part, dict):
		part = {}
	message = part.get('messageID')
	kind = event.get('type')
	if kind == 'text' and message is not None:
		texts.setdefault(message, []).append(str(part.get('text', '')))
		last_text = message
	elif kind == 'tool_use':
		tool_messages.add(message)
	elif kind == 'step_finish':
		last_step = message
		last_reason = part.get('reason')
if last_reason != 'stop':
	sys.exit('final step ended with reason %r' % (last_reason,))
if last_text is None or last_step != last_text or last_step in tool_messages:
	sys.exit('final step is not a text-only answer')
lines = '\n'.join(texts[last_text]).rstrip().splitlines()
if lines[-1:] != [marker]:
	sys.exit('review text does not end with the completion marker')
if not any(line.strip() for line in lines[:-1]):
	sys.exit('review text has no body before the completion marker')
try:
	fd = os.open(output + '.tmp', os.O_WRONLY | os.O_CREAT | os.O_EXCL |
	    os.O_NOFOLLOW, 0o600)
	with os.fdopen(fd, 'w', encoding='utf-8') as out:
		out.write('\n'.join(lines) + '\n')
	os.rename(output + '.tmp', output)
except OSError as failure:
	print('cannot write %s: %s' % (output, failure), file=sys.stderr)
	sys.exit(3)
EOF
}

attempt=1
while test "$attempt" -le "$attempts"; do
	events=$output.$attempt.jsonl
	(launch opencode run --pure -m "$model" --variant "$variant" \
	    --agent plan --format json \
	    'Read REVIEW.md and perform the requested review.') \
	    > "$events" 2> "$output.$attempt.stderr" < /dev/null &
	pid=$!
	started=$(date +%s)
	last_size=0
	last_change=$started
	# failure stays empty while opencode runs to its own exit; the watchdog
	# sets it when it stops an attempt.
	failure=
	while kill -0 "$pid" 2>/dev/null; do
		sleep "$poll"
		now=$(date +%s)
		size=$(event_bytes "$events")
		if test "$size" != "$last_size"; then
			last_size=$size
			last_change=$now
		fi
		if test "$size" = 0 && test $((now - started)) -ge "$startup_limit"; then
			failure="no event within ${startup_limit}s of starting"
		elif test $((now - last_change)) -ge "$stall_limit"; then
			failure="no event for ${stall_limit}s"
		elif test $((now - started)) -ge "$deadline"; then
			failure="attempt exceeded ${deadline}s"
		fi
		if test -n "$failure"; then
			stop_review
			break
		fi
	done
	if test -z "$failure"; then
		# The leader exited; helpers left in its group must not outlive it.
		kill -s KILL -- "-$pid" 2>/dev/null
		wait "$pid"
		status=$?
		pid=
		if test "$status" -ne 0; then
			failure="opencode exited with status $status"
		else
			reason=$(accept_review "$events" 2>&1)
			case $? in
			0)
				echo "opencode review accepted on attempt $attempt: $output"
				exit 0
				;;
			3)
				echo "$0: $reason" >&2
				exit 2
				;;
			esac
			failure=$reason
		fi
	fi
	echo "opencode review attempt $attempt/$attempts failed: $failure" >&2
	attempt=$((attempt + 1))
done
echo "opencode review FAILED after $attempts attempts; see $output.*.stderr" >&2
exit 1
