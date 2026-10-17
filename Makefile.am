# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
ACLOCAL_AMFLAGS = -I m4
AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/lib/core \
	-I$(top_srcdir)/compat -I$(top_srcdir)/cmd/fsimg \
	-I$(top_srcdir)/lib/meta -I$(top_srcdir)/lib/source \
	-I$(top_srcdir)/lib/write -I$(top_srcdir)/lib/io \
	-I$(top_srcdir)/lib/read -I$(top_srcdir)/lib/formats/fat
AM_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror
AM_CXXFLAGS = -std=c++11 -Wall -Wextra -Wpedantic -Werror
include_HEADERS = include/fsimg.h
dist_doc_DATA = LICENSE docs/references.md third-party/Unicode-LICENSE.txt \
	third-party/libarchive-msdosfs-LICENSE.txt
lib_LTLIBRARIES = libfsimg.la
libfsimg_la_SOURCES = lib/core/context.c lib/core/internal.h \
	lib/core/unicode.c lib/core/unicode.h \
	lib/io/io.c lib/output/publish.c lib/output/publish.h \
	lib/meta/meta.h lib/meta/store.c lib/meta/records.c lib/meta/sort.c \
	lib/write/writer.h lib/write/writer.c lib/write/catalog.c \
	lib/source/source.h lib/source/source.c lib/source/scan.c \
	lib/io/io.h lib/read/reader.h lib/read/reader.c \
	lib/formats/fat/fat.h lib/formats/fat/codec.c \
	lib/formats/fat/name-tables.h lib/formats/fat/read.c \
	lib/formats/fat/directory.c lib/formats/fat/verify.c \
	compat/host.c compat/host.h
libfsimg_la_LDFLAGS = -version-info 0:0:0 -no-undefined
check_PROGRAMS = test-core test-io test-meta test-meta-sort test-unicode \
	test-catalog test-source test-publish test-fat \
	test-sandbox header-c header-cxx
TESTS = $(check_PROGRAMS) test/fat-integration.sh
nodist_check_SCRIPTS = test/fat-integration.sh
test_core_SOURCES = test/core.c test/test.h
test_core_LDADD = libfsimg.la
test_io_SOURCES = test/io.c test/test.h
test_io_LDADD = libfsimg.la
test_meta_SOURCES = test/meta.c test/test.h
test_meta_LDADD = libfsimg.la
test_meta_sort_SOURCES = test/meta-sort.c test/test.h
test_meta_sort_LDADD = libfsimg.la
test_unicode_SOURCES = test/unicode.c test/test.h
test_unicode_LDADD = libfsimg.la
test_catalog_SOURCES = test/catalog.c test/test.h
test_catalog_LDADD = libfsimg.la
test_source_SOURCES = test/source.c test/test.h
test_source_LDADD = libfsimg.la
test_fat_SOURCES = test/fat.c test/test.h
test_fat_LDADD = libfsimg.la
test_publish_SOURCES = test/publish.c test/test.h
test_publish_LDADD = libfsimg.la
test_sandbox_SOURCES = test/sandbox.c cmd/fsimg/sandbox.c cmd/fsimg/sandbox.h
test_sandbox_LDADD = libfsimg.la
header_c_SOURCES = test/header.c
header_c_LDADD = libfsimg.la
header_cxx_SOURCES = test/header_cpp.cc
header_cxx_LDADD = libfsimg.la
EXTRA_DIST = LICENSE README.md NEWS AGENTS.md CLAUDE.md TODO.md \
	build-aux/check-ascii.sh build-aux/sanitize.sh \
	build-aux/fuzz.sh \
	docs/implementation-plan.md docs/build.md docs/platform-evidence.md \
	docs/references.md .github/workflows/check.yml \
	specs/README.md specs/principles.md specs/architecture.md \
	specs/style.md specs/design-api.md specs/design-storage.md \
	specs/design-sources.md specs/design-fat-reader.md \
	tools/gen-fat-tables.py third-party/Unicode-LICENSE.txt \
	test/fat-integration.sh.in fuzz/fat.c \
	specs/design-security.md specs/design-formats.md specs/design-cli.md
check-local: check-ascii
.PHONY: check-ascii check-asan check-ubsan check-lsan check-msan check-security
.PHONY: check-scale check-fuzz
.PHONY: check-integration
check-integration: test-fat test/fat-integration.sh
	./test/fat-integration.sh
check-scale: test-meta-sort test-catalog test-fat
	./test-meta-sort --scale
	./test-catalog --wide 100000
	./test-catalog --scale 100000
	./test-fat --scale
check-fuzz: test-fat
	$(SHELL) $(top_srcdir)/build-aux/fuzz.sh $(abs_top_srcdir)
check-ascii:
	$(SHELL) $(top_srcdir)/build-aux/check-ascii.sh $(top_srcdir)
check-asan:
	$(SHELL) $(top_srcdir)/build-aux/sanitize.sh asan $(abs_top_srcdir)
check-ubsan:
	$(SHELL) $(top_srcdir)/build-aux/sanitize.sh ubsan $(abs_top_srcdir)
check-lsan:
	$(SHELL) $(top_srcdir)/build-aux/sanitize.sh lsan $(abs_top_srcdir)
check-msan:
	$(SHELL) $(top_srcdir)/build-aux/sanitize.sh msan $(abs_top_srcdir)
check-security: test-sandbox test-publish
	./test-publish
	./test-sandbox; result=$$?; test $$result = 0 -o $$result = 77
