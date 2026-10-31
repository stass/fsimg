# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
#
# The environment of build-aux/linux-check.sh: the packages of the CI Linux
# job (.github/workflows/check.yml), installed with their recommendations as
# the job does (clang's sanitizer runtimes are one), plus what the GitHub
# runner image provides and the tests use as oracles. The build context holds
# only this file and the revision's test/spec Gemfile and Gemfile.lock.
FROM ubuntu:24.04
LABEL fsimg-linux-check=1
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN sed -i 's/^Types: deb$/Types: deb deb-src/' \
	    /etc/apt/sources.list.d/ubuntu.sources && \
	apt-get update && apt-get install -y \
	autoconf automake libtool make pkg-config clang llvm gcc g++ \
	zlib1g-dev mtools dosfstools exfatprogs fdisk gdisk zstd pigz \
	ruby ruby-dev ruby-bundler python3 git ca-certificates xz-utils file dpkg-dev
COPY build-msan-zlib.sh /usr/local/libexec/fsimg/build-msan-zlib.sh
RUN source_parent=$(mktemp -d) && cd "$source_parent" && \
	apt-get source zlib && \
	sh /usr/local/libexec/fsimg/build-msan-zlib.sh \
	    "$source_parent"/zlib-*/ /opt/fsimg-msan-zlib && \
	cd / && rm -rf "$source_parent" /var/lib/apt/lists/*
ENV MSAN_ZLIB=/opt/fsimg-msan-zlib
RUN useradd -m fsimg
USER fsimg
WORKDIR /home/fsimg
ENV BUNDLE_PATH=/home/fsimg/gems BUNDLE_FROZEN=true
COPY --chown=fsimg Gemfile Gemfile.lock gems-spec/
RUN bundle install --quiet --gemfile=gems-spec/Gemfile
