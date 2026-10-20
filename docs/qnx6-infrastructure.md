# QNX6 evidence infrastructure

The QNX6 work depends on material that never enters the repository: a QNX
6.5.0 virtual machine, the native captures it produced, the Porsche PCM 3.1
update disc and a measured head-unit hard disk. None of it is redistributable,
and the docs and tests refer to it only through environment variables. This
page records where it lives on the development workstation and how to drive
it, so that a later session can find and reuse it. The
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
| `~/qnx65-vm/tools/` | Harness scripts, described below |
| `~/qnx65-vm/workbench/` | The full working set of the original native and PCM analysis: decoders, disassembly listings (`*.s`, `rv-*`), symbol tables and intermediate images |
| `~/PCM-3.1-v4.76-disc-image.iso` | The PCM 3.1 update disc |
| `~/pcm31/iso/` | The update disc's `PCM31RDW400` variant tree and its `.def` file |
| `~/pcm31/PCM3_Emergency/`, `~/pcm31/ifs1/`, `~/pcm31/PCM3_IFS2/` | Unpacked recovery IFS, main IFS and notes on the HMI image container |
| `~/pcm31/bin/` | The head unit's own SH-4 `mkqnx6fs` and `chkqnx6fs`, for disassembly only |
| `~/pcm31/porschehdd.img` | The measured 100,030,242,816-byte PCM 3.1 HDD (read-only file mode); `~/porschehdd-fixed.img.zst` is a compressed copy |
| `~/pcm31/analysis/` | Per-partition listings of the measured HDD |
| `~/pcm31/tools/` | `ifs.py` (QNX IFS unpacker with LZO1X), `shrange.py` and `shdis.py` (SH-4 disassembly that resolves loaded literals and strings), `pcmq6.py`, `cmpldr.py` |
| `~/pcm31/pcm-qemu-plan.md` | Plan for booting the PCM recovery image in QEMU to run the head unit's own tools |

The NFI research fixture (`FSIMG_QNX6_F1`) is not on the workstation; the
research record names its source and pinned digest.

## The QNX 6.5.0 VM

`~/qnx65-vm/tools/vmstart.sh` boots the live CD headless with both disks:

```sh
qemu-system-x86_64 -accel hvf -m 1024 \
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
step needs checking. A VM left running keeps its serial shell, so check
`qemu.pid` before booting another.

Two helpers talk to a running VM:

* `qsh.py SOCKET 'COMMAND' [TIMEOUT]` runs one command in the serial shell,
  prints its output and exits with the guest command's status. It drains old
  output first, finds the end of output by a random marker, and caps output
  at 8 MiB.
* `mon.py SOCKET 'MONITOR-COMMAND'...` sends QEMU monitor commands and prints
  the replies.

## Captures

`capture.sh NAME 'GUEST-COMMANDS'` fills `/dev/hd0` with `0xaa` from inside
the guest, runs the commands (which format and populate `/dev/hd0`), syncs,
and saves `test0.img` as `captures/NAME.img.gz` with a `NAME.txt` record of
the date, guest `uname -a`, medium, commands and full output. `cap1.sh` does
the same for `/dev/hd1` on the sparse 5 GB `big.img`, saving zstd output. The
guest scripts `pop.sh` and `pop2.sh` populate a volume with the name, size
and metadata cases the research record cites. Names follow
`KIND-bBLOCKSIZE-ORDER-OPTION`, for example `empty-b2048-be-lfnoff` or
`pop2-b1024-le`.

The guest block cache does not see host writes to an attached disk. An image
built on the host, such as fsimg writer output, reaches the guest only through
a fresh guest view of the disk: copy it over `test0.img` and reboot the VM, or
attach it as a new drive the guest has not cached. Host-side edits between two
guest steps, such as the torn-header experiment, are recorded byte for byte in
the capture's `.txt` file.

## Decoders

`~/qnx65-vm/tools/q6dump.py`, `q6walk.py` and `q6ls.py` are Python decoders
written during the research, independent of `lib/formats/qnx6/`. They dump
headers, trees, inodes, directory slots and long names, mark which ranges the
formatter wrote over the `0xaa` fill, and list or extract files from a
partition of the HDD image. They are research aids, not test oracles.
