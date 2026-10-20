# QNX6 evidence infrastructure

The QNX6 work depends on material that never enters the repository: a QNX
6.5.0 virtual machine, the native captures it produced, the Porsche PCM 3.1
update disc and a measured head-unit hard disk. None of it is redistributable,
and the docs and tests refer to it only through environment variables. This
page records where it lives on the development workstation and how to drive
it with the scripts under `tools/qnx/`, so that a later session can find and
reuse it. The scripts are project-owned; no QNX software, firmware, capture or
disassembly listing enters the repository. The
[research record](qnx6-research.md) owns what the material shows; the
[QNX6 TODO](todo-qnx6.md) owns the work that remains, including turning the
harness below into a scripted, unattended part of the test suite.

All paths are on the owner's workstation (macOS, Homebrew QEMU and zstd).
Treat the HDD image and the existing captures as read-only evidence: write new
captures into a new directory, and never format or modify the originals.

## Layout

| Path | Contents |
| --- | --- |
| `~/qnx-6.5.0-x86-201007091524.iso` | QNX 6.5.0 x86 live CD, source X1 in the research record |
| `~/qnx65-vm/` | The VM's working directory: `test0.img` (64 MiB, guest `/dev/hd0`), `big.img` (5,033,164,800 bytes, guest `/dev/hd1`), `mon.sock` (QEMU monitor), `ser.sock` (guest serial shell), `qemu.pid` |
| `~/qnx65-vm/captures/` | Native captures: `NAME.img.gz` (64 MiB disk) or `NAME.img.zst` (5 GB disk), with `NAME.txt` (guest commands, tool output, provenance) and for some `NAME.decode.txt`; this is `FSIMG_QNX6_NATIVE` for `test/qnx6-integration.sh` |
| `~/qnx65-vm/bin/`, `~/qnx65-vm/boot/`, `~/qnx65-vm/rtkit/` | QNX 6.5.0 binaries copied out of the live CD for disassembly (source X2): `mkqnx6fs`, `chkqnx6fs`, `fs-qnx6.so`, `io-blk.so`, `fsysinfo`, and the other filesystem modules |
| `~/qnx65-vm/workbench/` | The full working set of the original native and PCM analysis: decoders, disassembly listings (`*.s`, `rv-*`), symbol tables and intermediate images |
| `~/PCM-3.1-v4.76-disc-image.iso` | The PCM 3.1 update disc |
| `~/pcm31/iso/` | The update disc's `PCM31RDW400` variant tree and its `.def` file |
| `~/pcm31/PCM3_Emergency/`, `~/pcm31/ifs1/`, `~/pcm31/PCM3_IFS2/` | Unpacked recovery IFS, main IFS and notes on the HMI image container |
| `~/pcm31/bin/` | The head unit's own SH-4 `mkqnx6fs` and `chkqnx6fs`, for disassembly only |
| `~/pcm31/porschehdd.img` | The measured 100,030,242,816-byte PCM 3.1 HDD (read-only file mode); `~/porschehdd-fixed.img.zst` is a compressed copy |
| `~/pcm31/analysis/` | Per-partition listings of the measured HDD |
| `~/pcm31/tools/` | Research scripts kept outside the repository: `ifs.py`, the QNX IFS unpacker used on the update disc, whose LZO1X decoder has no reviewed provenance, and `cmpldr.py`, a one-off comparison of the head unit's boot loader with the HDD's partition boot sectors; the other scripts there are earlier copies of `tools/qnx/pcm/` |
| `~/pcm31/pcm-qemu-plan.md` | Plan for booting the PCM recovery image in QEMU to run the head unit's own tools |

The NFI research fixture (`FSIMG_QNX6_F1`) is not on the workstation; the
research record names its source and pinned digest.

## The QNX 6.5.0 VM

The scripts in `tools/qnx/vm/` share `env.sh`, which takes the VM directory
from `FSIMG_QNX_VM` (default `~/qnx65-vm`), the live CD from `FSIMG_QNX_ISO`
and the directory for new captures from `FSIMG_QNX_CAPTURES` (default
`$FSIMG_QNX_VM/new-captures`, kept apart from the recorded evidence); they
write nothing into the source tree. Every script that sends keystrokes or
commands to the guest holds the lock directory `$FSIMG_QNX_VM/harness.lock`
for its whole run, and the Python helpers that talk to the sockets refuse to
run without it, so two captures or uploads cannot interleave in the one guest
shell. A script killed with `SIGKILL` leaves the lock, which names its pid,
to be removed by hand.

`vmstart.sh` refuses to start while `qemu.pid` names a live process, makes
the VM directory private (the monitor socket can run host commands), and
otherwise boots the live CD headless with both disks:

```sh
qemu-system-x86_64 -accel hvf -accel kvm -accel tcg -m 1024 \
    -cdrom ~/qnx-6.5.0-x86-201007091524.iso -boot d \
    -drive file=test0.img,format=raw,if=ide,index=0 \
    -drive file=big.img,format=raw,if=ide,index=1 \
    -monitor unix:mon.sock,server,nowait \
    -serial unix:ser.sock,server,nowait \
    -display none -vga std -daemonize -pidfile qemu.pid
```

The live CD has no serial console of its own, so `boot.sh` drives its
graphical boot through the monitor: it answers the boot prompts, logs in as
`root` and opens a terminal with timed `sendkey` and pointer events
(`type.py`, `click.py`). `vmstart.sh` then types
`stty -echo +edit </dev/ser1; on -t /dev/ser1 sh &` into that terminal, which
starts a shell on the serial port. The delays are fixed sleeps sized for this
host; `shot.sh` saves a screenshot (`screendump` plus `ppm2png.py`) when a
step needs checking, and a failed boot leaves QEMU running for that purpose.
Stop the VM with `kill "$(cat ~/qnx65-vm/qemu.pid)"`.

Two wrappers take the lock for one-off use of a running VM:

* `guest.sh 'COMMAND' [TIMEOUT]` runs one single-line command in the serial
  shell through `qsh.py`, prints its output and exits with the guest
  command's status. `qsh.py` drains old output first, finds the end of output
  by a random marker, and caps output at 8 MiB. A harness failure (timeout,
  lost connection, output cap) exits with status 124, a refused command or
  missing lock with 1; it may leave the guest command running, so nothing
  after it should trust the guest state. Scripts
  that record evidence use `qsh.py -s`, which reports the guest status in a
  file so a harness failure can never pass for one.
* `monitor.sh 'MONITOR-COMMAND'...` sends QEMU monitor commands through
  `mon.py` and prints the replies.

## Captures

`capture.sh [-d hd0|hd1] NAME 'GUEST-COMMANDS'` runs one native experiment and
keeps its evidence. On `hd0` (64 MiB `test0.img`, the default) it first fills
the disk with `0xaa` from inside the guest, so a decoder can tell which bytes
QNX wrote, and saves `NAME.img.gz`; on `hd1` (the sparse 5 GB `big.img`) the
disk is not reset, the record says so, and it saves `NAME.img.zst`. The
`NAME.txt` record holds the date, guest `uname -a`, medium, commands, full
output and the commands' exit status. The script refuses a name that already
exists and a command list containing a line break, and publishes only after
every harness step succeeded: the image first, then the record, each linked
into place so that neither overwrites an existing file. An image without its
record is an incomplete capture. The records in `~/qnx65-vm/captures` predate
the exit status: their `exit:` lines show the status of a trailing `sync`, so
their output, not that line, shows whether a guest command failed.

The guest scripts `tools/qnx/guest/pop.sh` and `pop2.sh` populate a volume
with the name, size and metadata cases the research record cites. `put.sh
LOCAL GUEST-PATH` copies such a script into the guest line by line over the
serial shell and compares checksums, for example:

```sh
tools/qnx/vm/put.sh tools/qnx/guest/pop.sh /tmp/pop.sh
tools/qnx/vm/capture.sh pop1-b1024-le \
    'mkqnx6fs -q -b 1024 -e little /dev/hd0; sh -x /tmp/pop.sh'
```

Names follow `KIND-bBLOCKSIZE-ORDER-OPTION`, for example
`empty-b2048-be-lfnoff` or `pop2-b1024-le`.

The guest block cache does not see host writes to an attached disk. An image
built on the host, such as fsimg writer output, reaches the guest only through
a fresh guest view of the disk: copy it over `test0.img` and reboot the VM, or
attach it as a new drive the guest has not cached. Host-side edits between two
guest steps, such as the torn-header experiment, are recorded byte for byte in
the capture's `.txt` file.

## Decoders and analysis

`tools/qnx/decode/q6dump.py`, `q6walk.py` and `q6ls.py` are Python decoders
written during the research, independent of `lib/formats/qnx6/`. They dump
headers, trees, inodes, directory slots and long names, mark which ranges the
formatter wrote over the `0xaa` fill, and list or extract files from a
partition of the HDD image. They are research aids, not test oracles;
`q6ls.py` prints names raw and writes extracted contents to the path it is
given, which belongs outside the source tree.

All four decoders trust their input: they do not bound tree depth, fan-out or
block references, so they are for the owner's own images and never part of
the test suite.

`tools/qnx/pcm/pcmq6.py IMAGE OFFSET LENGTH` prints the structure and
statistics of one classic QNX6 partition of a disk image, never file contents
(names only with `--names`). `shdis.py` and `shrange.py` disassemble SH-4
functions and address ranges of the head unit's binaries with Capstone's
`cstool` (and radare2's `rabin2` for symbols), resolving the literals and
strings each instruction loads.