# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
#
# The environment of build-aux/linux-check.sh: the packages of the CI Linux
# job (.github/workflows/check.yml), installed with their recommendations as
# the job does (clang's sanitizer runtimes are one), plus what the GitHub
# runner image provides and the tests use as oracles. The build context holds
# only this file and the revision's test/spec Gemfile and Gemfile.lock.
FROM ubuntu:24.04
LABEL fsimg-linux-check=1
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN apt-get update && apt-get install -y \
	autoconf automake libtool make pkg-config clang llvm gcc g++ \
	zlib1g-dev mtools dosfstools exfatprogs fdisk gdisk zstd pigz python3 \
	ruby ruby-dev ruby-bundler git ca-certificates xz-utils file && \
	rm -rf /var/lib/apt/lists/*
RUN useradd -m fsimg
USER fsimg
WORKDIR /home/fsimg
ENV BUNDLE_PATH=/home/fsimg/gems BUNDLE_FROZEN=true
COPY --chown=fsimg Gemfile Gemfile.lock gems-spec/
RUN bundle install --quiet --gemfile=gems-spec/Gemfile
