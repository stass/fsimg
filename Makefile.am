# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
ACLOCAL_AMFLAGS = -I m4
AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/lib/core \
	-I$(top_srcdir)/compat -I$(top_srcdir)/cmd/fsimg \
	-I$(top_srcdir)/lib/meta -I$(top_srcdir)/lib/source \
	-I$(top_srcdir)/lib/write -I$(top_srcdir)/lib/io \
	-I$(top_srcdir)/lib/output \
	-I$(top_srcdir)/lib/extract \
	-I$(top_srcdir)/lib/read -I$(top_srcdir)/lib/formats \
	-I$(top_srcdir)/lib/layers \
	-I$(top_srcdir)/lib/formats/fat -I$(top_srcdir)/lib/formats/exfat \
	-I$(top_srcdir)/lib/formats/qnx6 -I$(top_srcdir)/lib/formats/qnx4 \
	$(ZLIB_CFLAGS) \
	-I$(top_srcdir)/lib/formats/ifs \
	-I$(top_srcdir)/lib/codec
AM_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror
AM_CXXFLAGS = -std=c++11 -Wall -Wextra -Wpedantic -Werror
# The sanitizer and fuzz scripts run `make distdir` and `make check` as
# sub-makes. MAKE names this make program (gmake rather than BSD make, say),
# and their rules start with + to pass the jobserver. A + line runs even
# under make -n, so automake's am__make_dryrun keeps a dry run from
# replacing the previous sanitizer build.
SANITIZE_ENV = MAKE="$(MAKE)" MSAN_ZLIB="$(MSAN_ZLIB)"
include_HEADERS = include/fsimg.h
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libfsimg.pc
dist_man_MANS = docs/fsimg.1 docs/libfsimg.3
dist_doc_DATA = LICENSE README.md docs/examples.md docs/references.md third-party/Unicode-LICENSE.txt \
	third-party/FreeBSD-sha256-LICENSE.txt
examplesdir = $(datadir)/fsimg/examples
dist_examples_DATA = examples/example.h examples/create-tree.c examples/provider.c \
	examples/read-slice.c examples/extract.c
lib_LTLIBRARIES = libfsimg.la
noinst_LTLIBRARIES = libfsimg-core.la
libfsimg_la_SOURCES =
libfsimg_la_LIBADD = libfsimg-core.la
libfsimg_core_la_SOURCES = lib/core/context.c lib/core/internal.h \
	lib/core/fsimg-endian.h \
	lib/core/unicode.c lib/core/unicode.h lib/core/case-tables.h \
	lib/core/calendar.c lib/core/sha256.c lib/core/sha256.h \
	lib/core/settings.c lib/core/settings.h \
	lib/core/zlib-memory.c lib/core/zlib-memory.h \
	lib/formats/format.c lib/formats/format.h \
	lib/io/io.c lib/output/publish.c lib/output/publish.h \
	lib/output/write.c \
	lib/extract/extract.c lib/extract/extract.h \
	lib/extract/mtree-writer.c lib/extract/mtree-writer.h \
	lib/meta/meta.h lib/meta/store.c lib/meta/records.c lib/meta/sort.c \
	lib/meta/extent.c lib/meta/runs.c \
	lib/write/writer.h lib/write/writer.c lib/write/catalog.c \
	lib/write/emit.h lib/write/emit.c \
	lib/source/source.h lib/source/source.c lib/source/scan.c \
	lib/source/mtree.h lib/source/mtree.c lib/source/manifest.c \
	lib/source/generated.h lib/source/generated.c \
	lib/io/io.h lib/io/input.h lib/io/input.c \
	lib/read/directory.c lib/read/directory.h \
	lib/read/reader.h lib/read/reader.c \
	lib/read/selection.c lib/read/selection.h \
	lib/layers/layer.h lib/layers/stack.c lib/layers/types.c \
	lib/layers/image.c lib/layers/gpt.c lib/layers/mbr.c \
	lib/layers/plan.c lib/layers/output.c \
	lib/layers/chunks.c lib/layers/plist.c lib/layers/dmg.c \
	lib/layers/vhd.c lib/layers/simg.c lib/layers/transcode.c \
	lib/layers/spool.c lib/layers/gzip.c lib/layers/ifsboot.c \
	lib/layers/hbcifs.c \
	lib/formats/fat/fat.h lib/formats/fat/descriptor.c lib/formats/fat/codec.c \
	lib/formats/fat/name-tables.h lib/formats/fat/read.c \
	lib/formats/fat/directory.c lib/formats/fat/verify.c \
	lib/formats/fat/write.h lib/formats/fat/write-plan.c \
	lib/formats/fat/write-names.c lib/formats/fat/write-meta.c \
	lib/formats/fat/write-emit.c \
	lib/formats/exfat/exfat.h lib/formats/exfat/descriptor.c \
	lib/formats/exfat/codec.c \
	lib/formats/exfat/boot.c lib/formats/exfat/allocation.c \
	lib/formats/exfat/read.c lib/formats/exfat/directory.c \
	lib/formats/exfat/names.c lib/formats/exfat/verify.c \
	lib/formats/exfat/write-exfat.h lib/formats/exfat/write-plan.c \
	lib/formats/exfat/write-emit.c \
	lib/formats/qnx6/qnx6.h lib/formats/qnx6/descriptor.c \
	lib/formats/qnx6/codec.c lib/formats/qnx6/header.c \
	lib/formats/qnx6/tree.c lib/formats/qnx6/bitmap.c \
	lib/formats/qnx6/inode.c lib/formats/qnx6/names.c \
	lib/formats/qnx6/directory.c lib/formats/qnx6/read.c \
	lib/formats/qnx6/verify.c lib/formats/qnx6/write-qnx6.h \
	lib/formats/qnx6/write-plan.c lib/formats/qnx6/write-emit.c \
	lib/formats/qnx4/qnx4.h lib/formats/qnx4/descriptor.c \
	lib/formats/qnx4/codec.c lib/formats/qnx4/volume.c \
	lib/formats/qnx4/extent.c lib/formats/qnx4/bitmap.c \
	lib/formats/qnx4/inode.c lib/formats/qnx4/names.c \
	lib/formats/qnx4/directory.c lib/formats/qnx4/read.c \
	lib/formats/qnx4/verify.c lib/formats/qnx4/write-qnx4.h \
	lib/formats/qnx4/write-plan.c lib/formats/qnx4/write-emit.c \
	compat/host.c compat/host.h compat/private.c compat/private.h \
	lib/codec/codec.h lib/codec/lzo1x.c lib/codec/nrv2b.c \
	lib/formats/ifs/ifs.h lib/formats/ifs/descriptor.c \
	lib/formats/ifs/records.c lib/formats/ifs/volume.c \
	lib/formats/ifs/tree.c lib/formats/ifs/names.c \
	lib/formats/ifs/directory.c lib/formats/ifs/read.c \
	lib/formats/ifs/verify.c lib/formats/ifs/write-ifs.h \
	lib/formats/ifs/write-plan.c \
	lib/formats/ifs/write-emit.c
# The system zlib is the core's one third-party library; never vendored.
libfsimg_core_la_LIBADD = $(ZLIB_LIBS)
libfsimg_la_LDFLAGS = -version-info 0:0:0 -no-undefined
bin_PROGRAMS = fsimg
fsimg_SOURCES = cmd/fsimg/main.c cmd/fsimg/cli.h cmd/fsimg/options.c \
	cmd/fsimg/display.c cmd/fsimg/paths.c cmd/fsimg/job.c cmd/fsimg/worker.h \
	cmd/fsimg/protocol.c cmd/fsimg/protocol.h cmd/fsimg/worker-parent.c \
	cmd/fsimg/worker-child.c cmd/fsimg/worker-output.c \
	cmd/fsimg/sandbox.c cmd/fsimg/sandbox.h compat/clock.c compat/spawn.c
fsimg_LDADD = libfsimg-core.la
noinst_PROGRAMS = example-create-tree example-provider example-read-slice example-extract
noinst_PROGRAMS += fsimg-rss fsimg-rss-guard
fsimg_rss_SOURCES = $(fsimg_SOURCES) test/qualify-rss.c
# Qualification collects each existing child reap without a second supervisor.
fsimg_rss_CPPFLAGS = $(AM_CPPFLAGS) -Dmain=fsimg_measured_main \
	-Dwaitpid=fsimg_rss_waitpid
fsimg_rss_LDADD = libfsimg-core.la
fsimg_rss_guard_SOURCES = test/qualify-rss.c test/qualify-rss-guard.c test/test.h
fsimg_rss_guard_CPPFLAGS = $(AM_CPPFLAGS) -DFSIMG_RSS_GUARD
example_create_tree_SOURCES = examples/create-tree.c examples/example.h
example_create_tree_LDADD = libfsimg.la
example_provider_SOURCES = examples/provider.c examples/example.h
example_provider_LDADD = libfsimg.la
example_read_slice_SOURCES = examples/read-slice.c examples/example.h
example_read_slice_LDADD = libfsimg.la
example_extract_SOURCES = examples/extract.c examples/example.h
example_extract_LDADD = libfsimg.la
test_programs = test-core test-endian test-endian-unsigned-char \
	test-private test-io test-device test-meta test-meta-sort test-meta-runs test-unicode test-format \
	test-catalog test-source test-mtree test-manifest test-publish test-fat test-fat-write test-emit \
	test-driver-contracts test-layer-contracts test-zlib-memory \
	test-metadata-loss \
	test-sandbox test-sandbox-setup test-extract test-protocol test-worker test-staged-verify test-exfat \
	test-exfat-scale test-qnx6 test-qnx6-write test-qnx4 test-qnx4-write \
	test-ifs \
	test-ifs-write test-cli \
	test-qualify test-fragment test-posix test-layers test-layers-write test-scheme-planning \
	test-containers test-compression test-gzip-write test-holes test-settings \
	header-c \
	header-cxx test-codec test-hbcifs test-ifsboot test-ifsboot-write
# layers-compose is a helper of test/layers-integration.sh, not a test.
check_PROGRAMS = $(test_programs) layers-compose
TESTS = $(test_programs) test/fat-integration.sh test/exfat-integration.sh \
	test/qnx6-integration.sh test/qnx4-integration.sh \
	test/ifs-integration.sh \
	test/layers-integration.sh test/containers-integration.sh \
	test/compression-integration.sh test/cli.sh test/baseline.sh \
	test/spec.sh
nodist_check_SCRIPTS = test/fat-integration.sh test/exfat-integration.sh \
	test/qnx6-integration.sh test/qnx4-integration.sh \
	test/ifs-integration.sh \
	test/layers-integration.sh test/containers-integration.sh \
	test/compression-integration.sh test/cli.sh test/baseline.sh \
	test/spec.sh \
	test/codec-integration.sh
# The scalar codecs, built once with each signedness of plain char.
test_endian_SOURCES = test/endian.c test/test.h lib/core/fsimg-endian.h
test_endian_CFLAGS = $(AM_CFLAGS) -fsigned-char
test_endian_unsigned_char_SOURCES = $(test_endian_SOURCES)
test_endian_unsigned_char_CFLAGS = $(AM_CFLAGS) -funsigned-char
test_zlib_memory_SOURCES = test/zlib-memory.c test/test.h
test_zlib_memory_LDADD = libfsimg-core.la $(ZLIB_LIBS)
test_core_SOURCES = test/core.c test/test.h test/digest.c test/digest.h
test_core_LDADD = libfsimg.la
test_cli_SOURCES = test/cli.c test/test.h cmd/fsimg/cli.h cmd/fsimg/options.c \
	cmd/fsimg/display.c cmd/fsimg/paths.c cmd/fsimg/job.c cmd/fsimg/worker.h \
	cmd/fsimg/protocol.c cmd/fsimg/protocol.h cmd/fsimg/sandbox.c cmd/fsimg/sandbox.h \
	compat/clock.c
test_cli_LDADD = libfsimg-core.la
# The private-directory routine alone, its mkdirat hooked by the test; its
# own flags keep its compat objects apart from the library's.
test_private_SOURCES = test/private.c test/private-seam.c test/test.h \
	compat/host.c compat/host.h compat/private.h
test_private_CPPFLAGS = $(AM_CPPFLAGS)
test_qualify_SOURCES = test/qualify.c test/test.h
test_qualify_LDADD = libfsimg.la
test_fragment_SOURCES = test/fragment.c test/test.h
test_fragment_LDADD = libfsimg.la
test_io_SOURCES = test/io.c test/test.h
test_io_LDADD = libfsimg.la
test_device_SOURCES = test/device.c test/device-seam.c test/test.h \
	cmd/fsimg/sandbox.c cmd/fsimg/sandbox.h
test_device_LDADD = libfsimg-core.la
test_meta_SOURCES = test/meta.c test/test.h
test_meta_LDADD = libfsimg.la
test_meta_sort_SOURCES = test/meta-sort.c test/test.h
test_meta_sort_LDADD = libfsimg.la
test_meta_runs_SOURCES = test/meta-runs.c test/test.h
test_meta_runs_LDADD = libfsimg.la
test_unicode_SOURCES = test/unicode.c test/test.h
test_unicode_LDADD = libfsimg.la
test_format_SOURCES = test/format.c test/test.h
test_format_LDADD = libfsimg.la
test_layers_SOURCES = test/layers.c test/test.h
test_layers_LDADD = libfsimg-core.la
test_settings_SOURCES = test/settings.c test/test.h
test_settings_LDADD = libfsimg-core.la
test_layers_write_SOURCES = test/layers-write.c test/test.h
test_layers_write_LDADD = libfsimg-core.la
test_scheme_planning_SOURCES = test/scheme-planning.c test/test.h
test_scheme_planning_LDADD = libfsimg-core.la
test_containers_SOURCES = test/containers.c test/test.h
test_containers_LDADD = libfsimg-core.la
test_ifsboot_SOURCES = test/ifsboot.c test/test.h
test_ifsboot_LDADD = libfsimg-core.la
test_ifsboot_write_SOURCES = test/ifsboot-write.c test/test.h
test_ifsboot_write_LDADD = libfsimg-core.la
test_compression_SOURCES = test/compression.c test/test.h
test_compression_LDADD = libfsimg-core.la
test_hbcifs_SOURCES = test/hbcifs.c test/test.h
test_hbcifs_LDADD = libfsimg-core.la
test_gzip_write_SOURCES = test/gzip-write.c test/test.h
test_gzip_write_LDADD = libfsimg-core.la
test_holes_SOURCES = test/holes.c test/test.h
test_holes_LDADD = libfsimg-core.la
layers_compose_SOURCES = test/layers-compose.c
layers_compose_LDADD = libfsimg.la
test_posix_SOURCES = test/posix.c test/test.h
test_posix_LDADD = libfsimg.la
test_codec_SOURCES = test/codec.c test/test.h
# test/codec-integration.sh runs test-codec once per stream; linking the
# convenience library makes it a plain binary, not a libtool wrapper.
test_codec_LDADD = libfsimg-core.la
test_exfat_SOURCES = test/exfat.c test/test.h \
	test/baseline.c test/baseline.h test/digest.c test/digest.h
test_exfat_LDADD = libfsimg.la
test_qnx6_SOURCES = test/qnx6.c test/test.h
test_qnx6_LDADD = libfsimg.la
test_qnx6_write_SOURCES = test/qnx6-write.c test/test.h \
	test/digest.c test/digest.h
test_qnx6_write_LDADD = libfsimg.la
test_qnx4_SOURCES = test/qnx4.c test/test.h
test_qnx4_LDADD = libfsimg.la
test_qnx4_write_SOURCES = test/qnx4-write.c test/test.h
test_qnx4_write_LDADD = libfsimg.la
test_ifs_SOURCES = test/ifs.c test/test.h
test_ifs_LDADD = libfsimg.la
test_ifs_write_SOURCES = test/ifs-write.c test/test.h
test_ifs_write_LDADD = libfsimg.la
test_exfat_scale_SOURCES = test/exfat-scale.c test/test.h
test_exfat_scale_LDADD = libfsimg.la
test_catalog_SOURCES = test/catalog.c test/test.h test/entry-log.h
test_catalog_LDADD = libfsimg.la
test_source_SOURCES = test/source.c test/test.h test/socket.c test/socket.h
test_source_LDADD = libfsimg.la
test_mtree_SOURCES = test/mtree.c test/test.h
test_mtree_LDADD = libfsimg.la
test_manifest_SOURCES = test/manifest.c test/test.h test/digest.c \
	test/digest.h test/socket.c test/socket.h
test_manifest_LDADD = libfsimg.la
test_fat_SOURCES = test/fat.c test/test.h \
	test/baseline.c test/baseline.h test/digest.c test/digest.h
test_fat_LDADD = libfsimg.la
test_fat_write_SOURCES = test/fat-write.c test/test.h \
	test/baseline.c test/baseline.h test/digest.c test/digest.h
test_fat_write_LDADD = libfsimg.la
test_driver_contracts_SOURCES = test/driver-contracts.c test/contract.c \
	test/contract.h
test_driver_contracts_LDADD = libfsimg.la
test_layer_contracts_SOURCES = test/layer-contracts.c test/contract.c \
	test/contract.h
test_layer_contracts_LDADD = libfsimg.la
test_emit_SOURCES = test/emit.c test/test.h
test_emit_LDADD = libfsimg.la
test_metadata_loss_SOURCES = test/metadata-loss.c test/test.h
test_metadata_loss_LDADD = libfsimg.la
test_extract_SOURCES = test/extract.c test/test.h test/entry-log.h
test_extract_LDADD = libfsimg.la
test_protocol_SOURCES = test/protocol.c test/test.h \
	cmd/fsimg/protocol.c cmd/fsimg/protocol.h cmd/fsimg/job.c cmd/fsimg/worker.h
test_protocol_LDADD = libfsimg-core.la
test_worker_SOURCES = test/worker.c test/test.h test/socket.c test/socket.h \
	cmd/fsimg/worker.h cmd/fsimg/job.c cmd/fsimg/protocol.c cmd/fsimg/protocol.h \
	cmd/fsimg/worker-parent.c cmd/fsimg/worker-child.c cmd/fsimg/worker-output.c \
	cmd/fsimg/sandbox.c cmd/fsimg/sandbox.h compat/clock.c compat/spawn.c
# The clock routed through test/worker.c's hook, which can make the
# coordinator's monotonic clock fail.
test_worker_CPPFLAGS = $(AM_CPPFLAGS) -Dclock_gettime=fsimg_test_clock_gettime
test_worker_LDADD = libfsimg-core.la
test_staged_verify_SOURCES = $(fsimg_SOURCES) test/staged-verify.c test/test.h
test_staged_verify_CPPFLAGS = $(AM_CPPFLAGS) -Dmain=fsimg_test_command_main
test_staged_verify_LDADD = libfsimg-core.la
test_publish_SOURCES = test/publish.c test/test.h
test_publish_LDADD = libfsimg.la
test_sandbox_SOURCES = test/sandbox.c cmd/fsimg/sandbox.c cmd/fsimg/sandbox.h
test_sandbox_LDADD = libfsimg.la
test_sandbox_setup_SOURCES = test/sandbox-setup.c cmd/fsimg/sandbox.c
test_sandbox_setup_CPPFLAGS = $(AM_CPPFLAGS) \
	-Dsyscall=fsimg_test_syscall -Dprctl=fsimg_test_prctl
test_sandbox_setup_LDADD = libfsimg.la
header_c_SOURCES = test/header.c
header_c_LDADD = libfsimg.la
header_cxx_SOURCES = test/header_cpp.cc
header_cxx_LDADD = libfsimg.la
EXTRA_DIST = LICENSE README.md NEWS AGENTS.md CLAUDE.md TODO.md \
	build-aux/device-check.sh \
	libfsimg.pc.in docs/examples.md \
	build-aux/check-ascii.sh build-aux/check-dist.sh \
	build-aux/check-seek-holes.sh build-aux/sanitize.sh \
	build-aux/check-install.sh build-aux/check-installed.rb \
	build-aux/fuzz.sh \
	build-aux/build-fatcat.sh build-aux/fatcat-freebsd.patch \
	build-aux/build-msan-zlib.sh \
	build-aux/linux-check.sh build-aux/linux-check/Dockerfile \
	docs/implementation-plan.md docs/build.md docs/platform-evidence.md \
	docs/references.md docs/qualification.md .github/workflows/check.yml \
	docs/qnx6-research.md docs/qnx6-infrastructure.md docs/todo-qnx6.md \
	docs/spec-testing-plan.md docs/qnx4-plan.md docs/qnx4-research.md \
	docs/todo-qnx4.md docs/architecture-analysis.md docs/ifs-plan.md \
	docs/ifs-research.md docs/todo-ifs.md \
	specs/README.md specs/principles.md specs/architecture.md \
	specs/style.md specs/design-api.md specs/design-storage.md \
	specs/design-device-input.md \
	specs/design-sources.md specs/design-fat-reader.md specs/design-fat-writer.md \
	specs/design-extraction-worker.md specs/design-exfat.md specs/design-qnx6.md \
	specs/design-qnx6-driver.md specs/design-qnx4.md specs/design-qnx4-driver.md \
	specs/design-metadata.md specs/design-mtree.md specs/design-sizing.md \
	specs/design-settings.md specs/design-metadata-loss.md \
	specs/design-container-plans.md \
	specs/design-driver-contract-tests.md specs/design-verification-helpers.md \
	specs/design-binary-primitives.md \
	specs/design-scheme-planning.md specs/design-relic-generation.md \
	specs/design-generated-verification.md \
	specs/design-layers.md specs/design-content.md \
	specs/design-directories.md specs/design-emission.md \
	specs/design-holes.md specs/design-ifs.md specs/design-ifs-driver.md \
	specs/design-codecs.md specs/design-compression-libs.md \
	tools/qnx/decode/q6dump.py tools/qnx/decode/q6ls.py \
	tools/qnx/decode/q6walk.py tools/qnx/guest/pop.sh \
	tools/qnx/guest/pop2.sh tools/qnx/guest/pop4.sh tools/qnx/guest/manifest4.sh \
	tools/qnx/guest/qualify.sh tools/qnx/guest/qualify4.sh \
	tools/qnx/guest/popifs.sh tools/qnx/decode/ifsdump.py \
	tools/qnx/decode/ifschunks.py tools/qnx/decode/ifsflip.py \
	tools/qnx/decode/ifsqual.py tools/qnx/guest/qualifyifs.sh \
	tools/qnx/decode/ifscorrupt.py tools/qnx/vm/bootifs.sh \
	tools/qnx/oracle/oracle.c tools/qnx/oracle/build.sh \
	tools/qnx/vm/load.py \
	tools/qnx/decode/q4dump.py tools/qnx/pcm/pcmq4.py tools/qnx/pcm/pcmq6.py tools/qnx/pcm/shdis.py \
	tools/qnx/pcm/shrange.py tools/qnx/vm/boot.sh \
	tools/qnx/vm/capture.sh tools/qnx/vm/click.py tools/qnx/vm/env.sh \
	tools/qnx/vm/guest.sh tools/qnx/vm/mon.py tools/qnx/vm/monitor.sh \
	tools/qnx/vm/ppm2png.py tools/qnx/vm/put.sh \
	tools/qnx/vm/qsh.py tools/qnx/vm/shot.sh tools/qnx/vm/type.py \
	tools/qnx/vm/vmstart.sh \
	tools/gen-fat-tables.rb third-party/Unicode-LICENSE.txt \
	test/fat-integration.sh.in test/exfat-integration.sh.in test/qnx6-integration.sh.in \
	test/qnx4-integration.sh.in test/layers-integration.sh.in \
	test/containers-integration.sh.in test/compression-integration.sh.in \
	test/codec-integration.sh.in fuzz/fat.c fuzz/qnx6.c fuzz/qnx4.c \
	fuzz/protocol.c fuzz/image.c fuzz/container.c fuzz/plist.c fuzz/gzip.c \
	fuzz/mtree.c fuzz/codec.c fuzz/ifs.c test/ifs-integration.sh.in \
	test/ifs-native.rb \
	test/cli.sh.in test/qualify-cli.rb \
	test/cli/bundled_modes_test.rb \
	test/cli/change_directory_kinds_test.rb \
	test/cli/compressed_and_streams_test.rb \
	test/cli/compressed_create_test.rb \
	test/cli/container_create_test.rb \
	test/cli/containers_test.rb \
	test/cli/coordinator_worker_test.rb \
	test/cli/create_operands_test.rb \
	test/cli/examples_test.rb \
	test/cli/extract_here_test.rb \
	test/cli/extraction_acls_test.rb \
	test/cli/extraction_names_test.rb \
	test/cli/failed_creation_output_test.rb \
	test/cli/failures_test.rb \
	test/cli/fixtures.rb \
	test/cli/images.rb \
	test/cli/records.rb \
	test/cli/format_test.rb \
	test/cli/formats_test.rb \
	test/cli/helpers_test.rb \
	test/cli/human_commands_test.rb \
	test/cli/human_help_test.rb \
	test/cli/human_loss_bounds_test.rb \
	test/cli/ifs_create_test.rb \
	test/cli/ifs_test.rb \
	test/cli/ifsboot_create_test.rb \
	test/cli/manifest_generated_test.rb \
	test/cli/manifest_host_test.rb \
	test/cli/manifest_refusals_test.rb \
	test/cli/members_test.rb \
	test/cli/merged_operands_test.rb \
	test/cli/minimize_test.rb \
	test/cli/fat_root_test.rb \
	test/cli/mtree_export_test.rb \
	test/cli/mtree_failures_test.rb \
	test/cli/mtree_reporting_test.rb \
	test/cli/option_syntax_test.rb \
	test/cli/output_failures_test.rb \
	test/cli/output_inside_source_test.rb \
	test/cli/partitioned_create_test.rb \
	test/cli/partitioned_test.rb \
	test/cli/posix_metadata_test.rb \
	test/cli/process.rb \
	test/cli/qnx4_test.rb \
	test/cli/qnx6_test.rb \
	test/cli/run.rb \
	test/cli/tree_at_depth_limit_test.rb \
	test/cli/verbose_names_test.rb \
	test/baseline.sh.in test/baseline.txt \
	specs/design-security.md specs/design-formats.md specs/design-cli.md \
	specs/design-spec-testing.md test/spec.sh.in \
	test/spec/Gemfile test/spec/Gemfile.lock test/spec/run.rb \
	test/spec/bundle.rb test/spec/bin test/spec/cases test/spec/lib \
	test/spec/suite
# Every git-tracked file reaches the distribution; see check-dist.sh.
dist-hook:
	$(SHELL) $(top_srcdir)/build-aux/check-dist.sh $(top_srcdir) $(distdir)

check-local: check-ascii check-fstat check-seek-holes
.PHONY: check-ascii check-fstat check-seek-holes check-asan check-ubsan
.PHONY: check-lsan check-msan check-security
.PHONY: check-scale check-fuzz check-fuzz-extended check-cli-rss check-extended
.PHONY: check-integration baseline-record
.PHONY: check-spec check-spec-extended check-spec-deep
.PHONY: check-install
check-install: all
	+$(SHELL) $(top_srcdir)/build-aux/check-install.sh "$(abs_top_builddir)" \
	    "$(MAKE)" "$(CC)" "$(CXX)" "$(CPPFLAGS)" "$(CFLAGS)" "$(CXXFLAGS)" \
	    "$(LDFLAGS)" "$(PKG_CONFIG)" "$(RUBY)"
.PHONY: check-installed-consumers
CLEANFILES = provider-static provider-shared header-installed-cxx
check-installed-consumers:
	$(CC) "-I$(includedir)" $(CPPFLAGS) $(CFLAGS) -std=c11 $(LDFLAGS) \
	    "$(examplesdir)/provider.c" "$(libdir)/libfsimg.a" $(ZLIB_LIBS) -o provider-static
	$(CC) "-I$(includedir)" "-L$(libdir)" $(CPPFLAGS) $(CFLAGS) -std=c11 $(LDFLAGS) \
	    "$(examplesdir)/provider.c" -lfsimg -o provider-shared
	$(CXX) "-I$(includedir)" $(CPPFLAGS) $(CXXFLAGS) -std=c++11 $(LDFLAGS) \
	    "$(srcdir)/test/header_cpp.cc" "$(libdir)/libfsimg.a" $(ZLIB_LIBS) -o header-installed-cxx
	./header-installed-cxx
# The scripts call test programs and helpers, so every check program is a
# prerequisite rather than a list kept in step with the scripts.
check-integration: $(check_PROGRAMS) fsimg \
	    test/fat-integration.sh test/exfat-integration.sh \
	    test/qnx6-integration.sh test/qnx4-integration.sh \
	    test/layers-integration.sh test/containers-integration.sh \
	    test/compression-integration.sh \
	    test/codec-integration.sh test/ifs-integration.sh
	./test/fat-integration.sh
	./test/exfat-integration.sh
	./test/qnx6-integration.sh
	./test/qnx4-integration.sh
	./test/layers-integration.sh
	./test/containers-integration.sh
	./test/compression-integration.sh
	./test/codec-integration.sh; result=$$?; test $$result = 0 -o $$result = 77
	./test/ifs-integration.sh; result=$$?; test $$result = 0 -o $$result = 77
	./test-ifs-write --native; result=$$?; test $$result = 0 -o $$result = 77
# Regenerate the checked-in comparison baseline; test/baseline.sh only reads it.
baseline-record: test-fat test-fat-write test-exfat test/baseline.sh
	./test/baseline.sh --record > $(srcdir)/test/baseline.txt.new || \
	    { rm -f $(srcdir)/test/baseline.txt.new; exit 1; }
	mv $(srcdir)/test/baseline.txt.new $(srcdir)/test/baseline.txt
check-scale: test-meta-sort test-catalog test-fat test-fat-write test-exfat-scale \
	    test-qnx6 test-qnx6-write test-qnx4 test-qnx4-write test-ifs
	./test-meta-sort --scale
	./test-catalog --wide 100000
	./test-catalog --scale 100000
	./test-fat --scale
	./test-fat-write --scale
	./test-qnx6-write --scale
	./test-exfat-scale --huge; result=$$?; test $$result = 0 -o $$result = 77
	./test-qnx6 --scale; result=$$?; test $$result = 0 -o $$result = 77
	./test-qnx4 --scale; result=$$?; test $$result = 0 -o $$result = 77
	./test-qnx4-write --scale; result=$$?; test $$result = 0 -o $$result = 77
	MallocSpaceEfficient=1 ./test-ifs --scale
check-fuzz: test-fat test-exfat test-qnx6 test-qnx4 test-ifs test-codec \
	    test-protocol test-layers test-containers test-compression \
	    test-hbcifs test-mtree test-ifsboot test/spec.sh
	+@$(am__make_dryrun) || \
	    $(SANITIZE_ENV) $(SHELL) $(top_srcdir)/build-aux/fuzz.sh
check-fuzz-extended: test-fat test-exfat test-qnx6 test-qnx4 test-ifs \
	    test-codec test-protocol test-layers test-containers \
	    test-compression test-hbcifs test-mtree test-ifsboot test/spec.sh
	+@$(am__make_dryrun) || \
	    $(SANITIZE_ENV) FUZZ_SECONDS=600 $(SHELL) $(top_srcdir)/build-aux/fuzz.sh
check-cli-rss: fsimg-rss fsimg-rss-guard
	./fsimg-rss-guard; result=$$?; test $$result = 0 -o $$result = 77
	$(RUBY) $(top_srcdir)/test/qualify-cli.rb ./fsimg-rss 100000
check-extended: test-qualify test-fragment test-exfat-scale check-cli-rss \
	    check-spec-extended
	./test-qualify --catalog 10000000
	./test-qualify --wide 100000
	./test-fragment --fragments 1048576
	./test-exfat-scale --volume 33554432; result=$$?; test $$result = 0 -o $$result = 77
	./test-exfat-scale --volume 33554432000; result=$$?; test $$result = 0 -o $$result = 77
# The specification-driven tests by profile; quick also runs in check.
check-spec: fsimg test/spec.sh
	./test/spec.sh quick; result=$$?; test $$result = 0 -o $$result = 77
check-spec-extended: fsimg test/spec.sh
	./test/spec.sh nightly; result=$$?; test $$result = 0 -o $$result = 77
check-spec-deep: fsimg test/spec.sh
	./test/spec.sh deep; result=$$?; test $$result = 0 -o $$result = 77
check-ascii:
	$(SHELL) $(top_srcdir)/build-aux/check-ascii.sh $(top_srcdir)
# The tree passes, and a seeded SEEK_DATA outside compat/host.c fails.
check-seek-holes:
	$(SHELL) $(top_srcdir)/build-aux/check-seek-holes.sh $(top_srcdir)
	rm -rf seek-holes-seed && mkdir -p seek-holes-seed/lib && \
	    echo 'off_t data = SEEK_DATA;' > seek-holes-seed/lib/seed.c && \
	    if $(SHELL) $(top_srcdir)/build-aux/check-seek-holes.sh \
	    seek-holes-seed 2>/dev/null; then rm -rf seek-holes-seed; \
	    echo "check-seek-holes missed a seeded SEEK_DATA" >&2; exit 1; fi; \
	    rm -rf seek-holes-seed
# Only fsimg_host_fstat, in compat/host.c, calls fstat(2); compat/host.h
# says why. A grep failure, such as a glob that matches nothing, fails too.
check-fstat:
	@grep -n -E '(^|[^A-Za-z0-9_])fstat(64)?[[:space:]]*\(' \
	    $(top_srcdir)/lib/*/*.[ch] $(top_srcdir)/lib/formats/*/*.[ch] \
	    $(top_srcdir)/cmd/fsimg/*.[ch] $(top_srcdir)/compat/*.[ch] \
	    >check-fstat.out; test $$? -le 1 || { rm -f check-fstat.out; exit 1; }
	@if grep -v -E '/compat/host\.[ch]:' check-fstat.out; then \
		echo "call fsimg_host_fstat, not fstat (compat/host.h)" >&2; \
		rm -f check-fstat.out; exit 1; \
	fi; rm -f check-fstat.out
check-asan:
	+@$(am__make_dryrun) || \
	    $(SANITIZE_ENV) $(SHELL) $(top_srcdir)/build-aux/sanitize.sh asan
check-ubsan:
	+@$(am__make_dryrun) || \
	    $(SANITIZE_ENV) $(SHELL) $(top_srcdir)/build-aux/sanitize.sh ubsan
check-lsan:
	+@$(am__make_dryrun) || \
	    $(SANITIZE_ENV) $(SHELL) $(top_srcdir)/build-aux/sanitize.sh lsan
check-msan:
	+@$(am__make_dryrun) || \
	    $(SANITIZE_ENV) $(SHELL) $(top_srcdir)/build-aux/sanitize.sh msan
clean-local:
	-rm -rf sanitizer-asan sanitizer-ubsan sanitizer-lsan sanitizer-msan \
	    sanitizer-fuzz test/spec-results test/spec-cache seek-holes-seed
# Fuzz corpora persist across clean so later runs reuse their mutations.
distclean-local:
	-rm -rf fuzz-corpus exfat-fuzz-corpus qnx6-fuzz-corpus qnx4-fuzz-corpus \
	    protocol-fuzz-corpus image-fuzz-corpus container-fuzz-corpus \
	    plist-fuzz-corpus gzip-fuzz-corpus mtree-fuzz-corpus \
	    codec-fuzz-corpus ifs-fuzz-corpus
check-security: test-sandbox test-publish
	./test-publish
	./test-sandbox; result=$$?; test $$result = 0 -o $$result = 77
