# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
ACLOCAL_AMFLAGS = -I m4
AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/lib/core \
	-I$(top_srcdir)/compat -I$(top_srcdir)/cmd/fsimg \
	-I$(top_srcdir)/lib/meta
AM_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror
AM_CXXFLAGS = -std=c++11 -Wall -Wextra -Wpedantic -Werror
include_HEADERS = include/fsimg.h
lib_LTLIBRARIES = libfsimg.la
libfsimg_la_SOURCES = lib/core/context.c lib/core/internal.h \
	lib/io/io.c lib/output/publish.c lib/output/publish.h \
	lib/meta/meta.h lib/meta/store.c lib/meta/records.c lib/meta/sort.c \
	compat/host.c compat/host.h
libfsimg_la_LDFLAGS = -version-info 0:0:0 -no-undefined
check_PROGRAMS = test-core test-io test-meta test-meta-sort test-publish \
	test-sandbox header-c header-cxx
TESTS = $(check_PROGRAMS)
test_core_SOURCES = test/core.c test/test.h
test_core_LDADD = libfsimg.la
test_io_SOURCES = test/io.c test/test.h
test_io_LDADD = libfsimg.la
test_meta_SOURCES = test/meta.c test/test.h
test_meta_LDADD = libfsimg.la
test_meta_sort_SOURCES = test/meta-sort.c test/test.h
test_meta_sort_LDADD = libfsimg.la
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
	docs/implementation-plan.md docs/build.md docs/platform-evidence.md \
	docs/references.md .github/workflows/check.yml \
	specs/README.md specs/principles.md specs/architecture.md \
	specs/style.md specs/design-api.md specs/design-storage.md \
	specs/design-security.md specs/design-formats.md specs/design-cli.md
check-local: check-ascii
.PHONY: check-ascii check-asan check-ubsan check-lsan check-msan check-security
.PHONY: check-scale
check-scale: test-meta-sort
	./test-meta-sort --scale
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
