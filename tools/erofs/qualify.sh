#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
#
# Qualify fsimg's EROFS writer against independent readers
# (docs/erofs-research.md, fsimg writer output). For each output tree it
# builds the source, creates the image with fsimg, and checks the image
# with:
#
#   fsck   fsck.erofs --extract with no "<E>" line on standard error (1.9.4
#          exits 0 on a superblock checksum mismatch), and the extracted
#          tree against the source;
#   kernel a read-only loop mount in the colima VM, its find(1) listing
#          and SHA-256 sums against the source;
#   rust   rust-fs-erofs stat of every path and its recursive export
#          against the source;
#   fsimg  fsimg W, and fsimg t against the source.
#
# The root has no catalog entry: the writer gives it owner 0, mode 0755
# and the format time (design-erofs.md), so no oracle compares its
# metadata with the source directory's.
#
# Usage: qualify.sh [-l large-bytes] FSIMG WORKDIR [SCRATCH]
#
# WORKDIR holds the sources and images and must lie under $HOME, which the
# VM shares at the same path; SCRATCH (default WORKDIR) holds extractions.
# Each output's files are replaced as it is built. The oracles are found
# under $FSIMG_ORACLES (default ~/.local/share/fsimg-oracles). Each check
# prints PASS or FAIL; the exit status is 1 when any check failed.

set -eu

large_bytes=4294972296		# 2^32 + 5000: past a compact inode's size
while getopts l: flag; do
	case $flag in
	l)	large_bytes=$OPTARG ;;
	*)	exit 2 ;;
	esac
done
shift $((OPTIND - 1))
[ $# -ge 2 ] && [ $# -le 3 ] || {
	echo "usage: qualify.sh [-l large-bytes] FSIMG WORKDIR [SCRATCH]" >&2
	exit 2
}

here=$(cd "$(dirname "$0")" && pwd)
fsimg=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
mkdir -p "$2" "${3:-$2}"
work=$(cd "$2" && pwd -P)
scratch=$(cd "${3:-$2}" && pwd -P)
oracles=${FSIMG_ORACLES:-$HOME/.local/share/fsimg-oracles}
fsck=$oracles/erofs-utils/bin/fsck.erofs
rust=$oracles/rust-erofs/bin/rust-fs-erofs
uuid=0123abcd-4567-89ef-0123-456789abcdef
failures=0

# The VM re-parses the command line colima passes through ssh, and the
# mount runs as root, so the image path is limited to characters no shell
# interprets.
case $work in
"$HOME"/*) ;;
*)	echo "qualify.sh: WORKDIR must lie under \$HOME for the VM" >&2
	exit 2 ;;
esac
if printf '%s' "$work" | LC_ALL=C grep -q '[^A-Za-z0-9._/-]'; then
	echo "qualify.sh: WORKDIR may hold only [A-Za-z0-9._/-]" >&2
	exit 2
fi

manifest() {
	ruby "$here/manifest.rb" "$@"
}

result() {
	echo "$1 $2 $3${4:+: $4}"
	[ "$1" = PASS ] || failures=$((failures + 1))
}

# Run inside the VM as root: mount IMAGE read-only without devices,
# set-id or execution, list every path as manifest.rb's kernel reader
# expects, and unmount whatever happens.
vm_script='
set -eu
mnt=/mnt/fsimg-qualify
modprobe erofs
mkdir -p "$mnt"
mount -t erofs -o ro,loop,nodev,nosuid,noexec "$1" "$mnt"
trap "cd / && umount \"$mnt\"" EXIT
cd "$mnt"
find . -printf "%P\0%y\0%m\0%U\0%G\0%s\0%T@\0%l\0" \( -type f -exec sh -c "sha256sum <\"\$1\" | cut -c1-64" sh {} \; -o -printf "\n" \)
'

check_fsck() {
	name=$1 image=$2 out=$scratch/$1.fsck
	rm -rf "$out"
	if ! "$fsck" --extract="$out" --preserve "$image" \
	    >"$work/$name.fsck.log" 2>&1 ||
	    grep -q '<E>' "$work/$name.fsck.log"; then
		result FAIL "$name" fsck "$(grep '<E>' "$work/$name.fsck.log" | head -1)"
		return
	fi
	# A symbolic link extracted on the host takes the host's mode, so its
	# mode is not compared.
	if ! manifest tree "$out" >"$work/$name.fsck.m" \
	    2>>"$work/$name.fsck.log"; then
		result FAIL "$name" fsck "unreadable extraction"
		return
	fi
	compare "$name" fsck "$work/$name.fsck.m" \
	    type,size,sha256,target,mtime,mode:-symlink,uid,gid --no-root
	rm -rf "$out"
}

check_kernel() {
	name=$1 image=$2
	if ! printf '%s' "$vm_script" | colima ssh -- sudo sh -s "$image" \
	    >"$work/$name.kernel.raw" 2>"$work/$name.kernel.log"; then
		result FAIL "$name" kernel "$(head -1 "$work/$name.kernel.log")"
		return
	fi
	if ! manifest kernel "$work/$name.kernel.raw" >"$work/$name.kernel.m" \
	    2>>"$work/$name.kernel.log"; then
		result FAIL "$name" kernel "unreadable listing"
		return
	fi
	compare "$name" kernel "$work/$name.kernel.m" \
	    type,mode,uid,gid,size,mtime,sha256,target --no-root
}

check_rust() {
	name=$1 image=$2 out=$scratch/$1.rust
	rm -rf "$out"
	if ! "$rust" fs "$image" export --recursive --preserve=links / "$out" \
	    >"$work/$name.rust.log" 2>&1; then
		result FAIL "$name" rust "$(head -1 "$work/$name.rust.log")"
		return
	fi
	if ! manifest rust "$rust" "$image" "$out" >"$work/$name.rust.m" \
	    2>>"$work/$name.rust.log"; then
		result FAIL "$name" rust "stat failed"
		return
	fi
	compare "$name" rust "$work/$name.rust.m" \
	    type,mode,uid,gid,size,mtime,sha256,target --no-root
	rm -rf "$out"
}

check_fsimg() {
	name=$1 image=$2
	if ! "$fsimg" W -f "$image" >"$work/$name.verify.log" 2>&1; then
		result FAIL "$name" fsimg "W: $(tail -1 "$work/$name.verify.log")"
		return
	fi
	if ! "$fsimg" t --json -f "$image" >"$work/$name.list" \
	    2>"$work/$name.list.log" ||
	    ! manifest fsimg "$work/$name.list" >"$work/$name.fsimg.m" \
	    2>>"$work/$name.list.log"; then
		result FAIL "$name" fsimg "t: $(tail -1 "$work/$name.list.log")"
		return
	fi
	compare "$name" fsimg "$work/$name.fsimg.m" \
	    type,mode,uid,gid,size,mtime --no-root
}

compare() {
	if manifest compare "$work/$1.source.m" "$3" "$4" ${5:-} \
	    2>"$work/$1.$2.diff"; then
		result PASS "$1" "$2"
	else
		result FAIL "$1" "$2" "$(head -1 "$work/$1.$2.diff")"
	fi
}

# Create NAME's image from its source with the given layout, then run
# every oracle on the filesystem: the image itself, or, for a stack with a
# partition scheme (a layout such as erofs/gpt, which holds a slash), the
# first partition copied out.
qualify() {
	name=$1 layout=$2
	shift 2
	src=$work/$name.src image=$work/$name.img
	rm -f "$image"
	if ! "$fsimg" c -p -o uuid=$uuid -F "$layout" "$@" -f "$image" \
	    -C "$src" . >"$work/$name.create.log" 2>&1; then
		result FAIL "$name" create "$(tail -1 "$work/$name.create.log")"
		return
	fi
	if ! manifest tree "$src" >"$work/$name.source.m"; then
		result FAIL "$name" source "unreadable source"
		return
	fi
	case $layout in
	*/*)	if ! extract_partition "$image" "$work/$name.part"; then
			result FAIL "$name" partition "no partition copied out"
			return
		fi
		image=$work/$name.part ;;
	esac
	check_fsck "$name" "$image"
	check_kernel "$name" "$image"
	check_rust "$name" "$image"
	check_fsimg "$name" "$image"
}

extract_partition() {
	"$fsimg" i --json -f "$1" 2>/dev/null | ruby -rjson -e '
	    part = $stdin.each_line.map { JSON.parse(_1) }
	        .find { _1["type"] == "partition" } or abort("no partition")
	    puts [part["offset_bytes"], part["length_bytes"]]
	        .map { Integer(_1) / 512 }.join(" ")' >"$2.extent" &&
	    read -r skip count <"$2.extent" &&
	    dd if="$1" of="$2" bs=512 skip="$skip" count="$count" 2>/dev/null
}

# Sources. Content is deterministic; times are the creation times, with
# fractional seconds, except one file's whole-second time in 2000.
fresh() {
	rm -rf "$work/$1.src"
	mkdir -p "$work/$1.src"
	echo "$work/$1.src"
}

count() {
	awk -v n="$1" 'BEGIN { for (i = 1; i <= n; i++) print i }'
}

bytes() {
	ruby -e 'print((0...Integer(ARGV[0])).map { (_1 * 7 + 3) % 251 }.pack("C*"))' "$1"
}

build_empty() {
	fresh empty-tree >/dev/null
}

build_small() {
	s=$(fresh small)
	mkdir -p "$s/dir/sub"
	for n in 0 1 31 32 4064 4095 4096 4097 9000; do
		bytes $n >"$s/dir/f$n"
	done
	printf 'hello\n' >"$s/hello"
	ln -s dir/f1 "$s/link"
	chmod 0600 "$s/dir/f1"
	chmod 0750 "$s/dir/sub"
	touch -t 200001020304.05 "$s/dir/f0"
	touch -t 200001020304.05 "$s/dir"
}

build_deep() {
	s=$(fresh deep)
	d=$s
	for i in $(count 40); do
		d=$d/level$i
	done
	mkdir -p "$d"
	printf 'bottom\n' >"$d/file"
}

build_wide() {
	s=$(fresh wide)
	mkdir "$s/many" "$s/long"
	for i in $(count 1500); do
		: >"$s/many/entry-$i"
	done
	for c in a b c d e f g h i j k l m n o p q r s t; do
		: >"$s/long/$(printf "%0255d" 0 | tr 0 "$c")"
	done
}

build_large() {
	s=$(fresh large)
	bytes 1048577 >"$s/mid"
	printf 'head' >"$s/sparse"
	dd if=/dev/zero of="$s/sparse" bs=1 count=0 seek=$((large_bytes - 4)) \
	    2>/dev/null
	printf 'tail' >>"$s/sparse"
}

build_kinds() {
	s=$(fresh kinds)
	mkdir "$s/empty-dir"
	: >"$s/empty-file"
	bytes 5000 >"$s/regular"
	ln "$s/regular" "$s/hard-link"
	ln -s regular "$s/short-link"
	ln -s /absolute/target "$s/absolute-link"
	ln -s missing "$s/dangling-link"
	ln -s "$(printf "%01000d" 0 | tr 0 x)" "$s/long-link"
}

build_names() {
	s=$(fresh names)
	for n in ' space' '!' '#hash' '$' '%' '&' "'" '(' ')' '*' '+' ',' \
	    '-dash' '.hidden' '..dots' 'a' 'Z' "caf$(printf '\303\251')"; do
		printf '%s\n' "$n" >"$s/$n"
	done
	mkdir "$s/!dir" "$s/-dir"
	printf 'inner\n' >"$s/!dir/ "
}

build_empty
build_small
build_deep
build_wide
build_large
build_kinds
build_names

qualify empty-tree erofs
qualify small erofs
qualify deep erofs
qualify wide erofs
qualify large erofs
qualify kinds erofs
qualify names erofs
cp -R "$work/small.src" "$work/gpt.src"
cp -R "$work/small.src" "$work/mbr.src"
qualify gpt erofs/gpt -o gpt:disk=8M
qualify mbr erofs/mbr -o mbr:disk=8M

# The writer profile admits only 4096-byte blocks (design-erofs.md,
# Volume), so the command refuses the smaller ones as outside it.
for b in 512 1024 2048; do
	if "$fsimg" c -n -o uuid=$uuid -F erofs -o cluster=$b \
	    -C "$work/small.src" . >/dev/null 2>"$work/block-$b.log"; then
		result FAIL "block-$b" refusal "accepted"
	elif grep -q 'outside the selected writer profile' "$work/block-$b.log"; then
		result PASS "block-$b" refusal
	else
		result FAIL "block-$b" refusal "$(tail -1 "$work/block-$b.log")"
	fi
done

echo "$failures failed"
[ "$failures" -eq 0 ]
