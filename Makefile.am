# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
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
	$(ZLIB_CFLAGS)
AM_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror
AM_CXXFLAGS = -std=c++11 -Wall -Wextra -Wpedantic -Werror
# The sanitizer and fuzz scripts run `make distdir` and `make check` as
# sub-makes. MAKE names this make program (gmake rather than BSD make, say),
# and their rules start with + to pass the jobserver. A + line runs even
# under make -n, so automake's am__make_dryrun keeps a dry run from
# replacing the previous sanitizer build.
SANITIZE_ENV = MAKE="$(MAKE)"
include_HEADERS = include/fsimg.h
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libfsimg.pc
dist_man_MANS = docs/fsimg.1 docs/libfsimg.3
dist_doc_DATA = LICENSE README.md docs/examples.md docs/references.md third-party/Unicode-LICENSE.txt \
	third-party/libarchive-msdosfs-LICENSE.txt
examplesdir = $(datadir)/fsimg/examples
dist_examples_DATA = examples/example.h examples/create-tree.c examples/provider.c \
	examples/read-slice.c examples/extract.c
lib_LTLIBRARIES = libfsimg.la
noinst_LTLIBRARIES = libfsimg-core.la
libfsimg_la_SOURCES =
libfsimg_la_LIBADD = libfsimg-core.la
libfsimg_core_la_SOURCES = lib/core/context.c lib/core/internal.h \
	lib/core/unicode.c lib/core/unicode.h lib/core/case-tables.h \
	lib/core/calendar.c \
	lib/formats/format.c lib/formats/format.h \
	lib/io/io.c lib/output/publish.c lib/output/publish.h \
	lib/output/write.c \
	lib/extract/extract.c lib/extract/extract.h \
	lib/meta/meta.h lib/meta/store.c lib/meta/records.c lib/meta/sort.c \
	lib/meta/extent.c \
	lib/write/writer.h lib/write/writer.c lib/write/catalog.c \
	lib/write/emit.h lib/write/emit.c \
	lib/source/source.h lib/source/source.c lib/source/scan.c \
	lib/io/io.h lib/read/directory.c lib/read/directory.h \
	lib/read/reader.h lib/read/reader.c \
	lib/read/selection.c lib/read/selection.h \
	lib/layers/layer.h lib/layers/stack.c lib/layers/types.c \
	lib/layers/image.c lib/layers/gpt.c lib/layers/mbr.c \
	lib/layers/plan.c lib/layers/output.c \
	lib/layers/chunks.c lib/layers/plist.c lib/layers/dmg.c \
	lib/layers/vhd.c lib/layers/simg.c lib/layers/transcode.c \
	lib/layers/spool.c lib/layers/gzip.c \
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
	compat/host.c compat/host.h
# The system zlib is the core's one third-party library; never vendored.
libfsimg_core_la_LIBADD = $(ZLIB_LIBS)
libfsimg_la_LDFLAGS = -version-info 0:0:0 -no-undefined
bin_PROGRAMS = fsimg
fsimg_SOURCES = cmd/fsimg/main.c cmd/fsimg/cli.h cmd/fsimg/options.c \
	cmd/fsimg/display.c cmd/fsimg/paths.c cmd/fsimg/job.c cmd/fsimg/worker.h \
	cmd/fsimg/protocol.c cmd/fsimg/protocol.h cmd/fsimg/worker-parent.c \
	cmd/fsimg/worker-child.c cmd/fsimg/worker-output.c \
	cmd/fsimg/sandbox.c cmd/fsimg/sandbox.h compat/private.c \
	compat/private.h compat/spawn.c
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
test_programs = test-core test-io test-meta test-meta-sort test-unicode test-format \
	test-catalog test-source test-publish test-fat test-fat-write test-emit \
	test-sandbox test-sandbox-setup test-extract test-protocol test-worker test-exfat \
	test-exfat-scale test-qnx6 test-qnx6-write test-qnx4 test-qnx4-write test-cli \
	test-qualify test-fragment test-posix test-layers test-layers-write \
	test-containers test-compression test-gzip-write header-c header-cxx
# layers-compose is a helper of test/layers-integration.sh, not a test.
check_PROGRAMS = $(test_programs) layers-compose
TESTS = $(test_programs) test/fat-integration.sh test/exfat-integration.sh \
	test/qnx6-integration.sh test/qnx4-integration.sh test/layers-integration.sh \
	test/containers-integration.sh test/compression-integration.sh \
	test/cli.sh test/baseline.sh test/spec.sh
nodist_check_SCRIPTS = test/fat-integration.sh test/exfat-integration.sh \
	test/qnx6-integration.sh test/qnx4-integration.sh test/layers-integration.sh \
	test/containers-integration.sh test/compression-integration.sh \
	test/cli.sh test/baseline.sh test/spec.sh
test_core_SOURCES = test/core.c test/test.h test/digest.c test/digest.h
test_core_LDADD = libfsimg.la
test_cli_SOURCES = test/cli.c test/test.h cmd/fsimg/cli.h cmd/fsimg/options.c \
	cmd/fsimg/display.c cmd/fsimg/paths.c cmd/fsimg/job.c cmd/fsimg/worker.h \
	cmd/fsimg/protocol.c cmd/fsimg/protocol.h cmd/fsimg/sandbox.c cmd/fsimg/sandbox.h \
	compat/private.c compat/private.h
test_cli_LDADD = libfsimg-core.la
test_qualify_SOURCES = test/qualify.c test/test.h
test_qualify_LDADD = libfsimg.la
test_fragment_SOURCES = test/fragment.c test/test.h
test_fragment_LDADD = libfsimg.la
test_io_SOURCES = test/io.c test/test.h
test_io_LDADD = libfsimg.la
test_meta_SOURCES = test/meta.c test/test.h
test_meta_LDADD = libfsimg.la
test_meta_sort_SOURCES = test/meta-sort.c test/test.h
test_meta_sort_LDADD = libfsimg.la
test_unicode_SOURCES = test/unicode.c test/test.h
test_unicode_LDADD = libfsimg.la
test_format_SOURCES = test/format.c test/test.h
test_format_LDADD = libfsimg.la
test_layers_SOURCES = test/layers.c test/test.h
test_layers_LDADD = libfsimg-core.la
test_layers_write_SOURCES = test/layers-write.c test/test.h
test_layers_write_LDADD = libfsimg-core.la
test_containers_SOURCES = test/containers.c test/test.h
test_containers_LDADD = libfsimg-core.la
test_compression_SOURCES = test/compression.c test/test.h
test_compression_LDADD = libfsimg-core.la
test_gzip_write_SOURCES = test/gzip-write.c test/test.h
test_gzip_write_LDADD = libfsimg-core.la
layers_compose_SOURCES = test/layers-compose.c
layers_compose_LDADD = libfsimg.la
test_posix_SOURCES = test/posix.c test/test.h
test_posix_LDADD = libfsimg.la
test_exfat_SOURCES = test/exfat.c test/test.h \
	test/baseline.c test/baseline.h test/digest.c test/digest.h
test_exfat_LDADD = libfsimg.la
test_qnx6_SOURCES = test/qnx6.c test/test.h
test_qnx6_LDADD = libfsimg.la
test_qnx6_write_SOURCES = test/qnx6-write.c test/test.h
test_qnx6_write_LDADD = libfsimg.la
test_qnx4_SOURCES = test/qnx4.c test/test.h
test_qnx4_LDADD = libfsimg.la
test_qnx4_write_SOURCES = test/qnx4-write.c test/test.h
test_qnx4_write_LDADD = libfsimg.la
test_exfat_scale_SOURCES = test/exfat-scale.c test/test.h
test_exfat_scale_LDADD = libfsimg.la
test_catalog_SOURCES = test/catalog.c test/test.h
test_catalog_LDADD = libfsimg.la
test_source_SOURCES = test/source.c test/test.h
test_source_LDADD = libfsimg.la
test_fat_SOURCES = test/fat.c test/test.h \
	test/baseline.c test/baseline.h test/digest.c test/digest.h
test_fat_LDADD = libfsimg.la
test_fat_write_SOURCES = test/fat-write.c test/test.h \
	test/baseline.c test/baseline.h test/digest.c test/digest.h
test_fat_write_LDADD = libfsimg.la
test_emit_SOURCES = test/emit.c test/test.h
test_emit_LDADD = libfsimg.la
test_extract_SOURCES = test/extract.c test/test.h
test_extract_LDADD = libfsimg.la
test_protocol_SOURCES = test/protocol.c test/test.h \
	cmd/fsimg/protocol.c cmd/fsimg/protocol.h cmd/fsimg/job.c cmd/fsimg/worker.h
test_protocol_LDADD = libfsimg-core.la
test_worker_SOURCES = test/worker.c test/test.h \
	cmd/fsimg/worker.h cmd/fsimg/job.c cmd/fsimg/protocol.c cmd/fsimg/protocol.h \
	cmd/fsimg/worker-parent.c cmd/fsimg/worker-child.c cmd/fsimg/worker-output.c \
	cmd/fsimg/sandbox.c cmd/fsimg/sandbox.h compat/spawn.c
test_worker_LDADD = libfsimg-core.la
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
	libfsimg.pc.in docs/examples.md \
	build-aux/check-ascii.sh build-aux/sanitize.sh \
	build-aux/check-install.sh build-aux/check-installed.py \
	build-aux/fuzz.sh \
	build-aux/build-fatcat.sh build-aux/fatcat-freebsd.patch \
	build-aux/opencode-review.sh \
	docs/implementation-plan.md docs/build.md docs/platform-evidence.md \
	docs/references.md docs/qualification.md .github/workflows/check.yml \
	docs/qnx6-research.md docs/qnx6-infrastructure.md docs/todo-qnx6.md \
	docs/spec-testing-plan.md docs/qnx4-plan.md docs/qnx4-research.md \
	docs/todo-qnx4.md \
	specs/README.md specs/principles.md specs/architecture.md \
	specs/style.md specs/design-api.md specs/design-storage.md \
	specs/design-sources.md specs/design-fat-reader.md specs/design-fat-writer.md \
	specs/design-extraction-worker.md specs/design-exfat.md specs/design-qnx6.md \
	specs/design-qnx6-driver.md specs/design-qnx4.md specs/design-qnx4-driver.md \
	specs/design-metadata.md specs/design-sizing.md specs/design-layers.md \
	specs/design-content.md specs/design-directories.md specs/design-emission.md \
	tools/qnx/decode/q6dump.py tools/qnx/decode/q6ls.py \
	tools/qnx/decode/q6walk.py tools/qnx/guest/pop.sh \
	tools/qnx/guest/pop2.sh tools/qnx/guest/pop4.sh tools/qnx/guest/manifest4.sh \
	tools/qnx/guest/qualify4.sh \
	tools/qnx/decode/q4dump.py tools/qnx/pcm/pcmq4.py tools/qnx/pcm/pcmq6.py tools/qnx/pcm/shdis.py \
	tools/qnx/pcm/shrange.py tools/qnx/vm/boot.sh \
	tools/qnx/vm/capture.sh tools/qnx/vm/click.py tools/qnx/vm/env.sh \
	tools/qnx/vm/guest.sh tools/qnx/vm/mon.py tools/qnx/vm/monitor.sh \
	tools/qnx/vm/ppm2png.py tools/qnx/vm/put.sh \
	tools/qnx/vm/qsh.py tools/qnx/vm/shot.sh tools/qnx/vm/type.py \
	tools/qnx/vm/vmstart.sh \
	tools/gen-fat-tables.py third-party/Unicode-LICENSE.txt \
	test/fat-integration.sh.in test/exfat-integration.sh.in test/qnx6-integration.sh.in \
	test/qnx4-integration.sh.in test/layers-integration.sh.in \
	test/containers-integration.sh.in test/compression-integration.sh.in \
	fuzz/fat.c fuzz/qnx6.c fuzz/qnx4.c fuzz/protocol.c fuzz/image.c \
	fuzz/container.c fuzz/plist.c fuzz/gzip.c \
	test/cli.sh.in test/cli.py test/qualify-cli.py \
	test/baseline.sh.in test/baseline.txt \
	specs/design-security.md specs/design-formats.md specs/design-cli.md \
	specs/design-spec-testing.md test/spec.sh.in \
	test/spec/Gemfile test/spec/Gemfile.lock test/spec/run.rb \
	test/spec/bundle.rb test/spec/bin test/spec/cases test/spec/lib \
	test/spec/suite
check-local: check-ascii
.PHONY: check-ascii check-asan check-ubsan check-lsan check-msan check-security
.PHONY: check-scale check-fuzz check-fuzz-extended check-cli-rss check-extended
.PHONY: check-integration baseline-record
.PHONY: check-spec check-spec-extended check-spec-deep
.PHONY: check-install
check-install: all
	+$(SHELL) $(top_srcdir)/build-aux/check-install.sh "$(abs_top_builddir)" \
	    "$(MAKE)" "$(CC)" "$(CXX)" "$(CPPFLAGS)" "$(CFLAGS)" "$(CXXFLAGS)" \
	    "$(LDFLAGS)" "$(PKG_CONFIG)" "$(PYTHON3)"
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
check-integration: test-fat test-fat-write test-exfat test-qnx6 test-qnx6-write \
	    test-qnx4 test-qnx4-write fsimg test/fat-integration.sh \
	    test/exfat-integration.sh test/qnx6-integration.sh \
	    test/qnx4-integration.sh test/layers-integration.sh \
	    test/containers-integration.sh test/compression-integration.sh
	./test/fat-integration.sh
	./test/exfat-integration.sh
	./test/qnx6-integration.sh
	./test/qnx4-integration.sh
	./test/layers-integration.sh
	./test/containers-integration.sh
	./test/compression-integration.sh
# Regenerate the checked-in comparison baseline; test/baseline.sh only reads it.
baseline-record: test-fat test-fat-write test-exfat test/baseline.sh
	./test/baseline.sh --record > $(srcdir)/test/baseline.txt.new || \
	    { rm -f $(srcdir)/test/baseline.txt.new; exit 1; }
	mv $(srcdir)/test/baseline.txt.new $(srcdir)/test/baseline.txt
check-scale: test-meta-sort test-catalog test-fat test-fat-write test-exfat-scale \
	    test-qnx6 test-qnx6-write test-qnx4 test-qnx4-write
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
check-fuzz: test-fat test-exfat test-qnx6 test-qnx4 test-protocol test-layers \
	    test-containers test-compression test/spec.sh
	+@$(am__make_dryrun) || \
	    $(SANITIZE_ENV) $(SHELL) $(top_srcdir)/build-aux/fuzz.sh
check-fuzz-extended: test-fat test-exfat test-qnx6 test-qnx4 test-protocol \
	    test-layers test-containers test-compression test/spec.sh
	+@$(am__make_dryrun) || \
	    $(SANITIZE_ENV) FUZZ_SECONDS=600 $(SHELL) $(top_srcdir)/build-aux/fuzz.sh
check-cli-rss: fsimg-rss fsimg-rss-guard
	./fsimg-rss-guard; result=$$?; test $$result = 0 -o $$result = 77
	$(PYTHON3) $(top_srcdir)/test/qualify-cli.py ./fsimg-rss 100000
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
	    sanitizer-fuzz test/spec-results test/spec-cache
# Fuzz corpora persist across clean so later runs reuse their mutations.
distclean-local:
	-rm -rf fuzz-corpus exfat-fuzz-corpus qnx6-fuzz-corpus qnx4-fuzz-corpus \
	    protocol-fuzz-corpus image-fuzz-corpus container-fuzz-corpus \
	    plist-fuzz-corpus gzip-fuzz-corpus
check-security: test-sandbox test-publish
	./test-publish
	./test-sandbox; result=$$?; test $$result = 0 -o $$result = 77
