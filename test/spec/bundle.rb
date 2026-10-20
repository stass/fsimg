# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

# Loads the harness for run.rb and the bin/ commands: the locked gems, then
# lib/ on the load path.
#
# Only the locked gems: a run never rewrites Gemfile.lock, installs a gem or
# reaches the network. Temporary settings outrank every Bundler
# configuration, a local test/spec/.bundle/config included. Any other
# failure to load the bundle, such as a Gemfile changed without relocking,
# raises and fails the run.
# Ruby matches a rescue list in order, so Bundler::GemNotFound is not looked
# up when LoadError reports a missing Bundler.
begin
  require "bundler"
  Bundler.settings.temporary(frozen: true, auto_install: false) { Bundler.setup }
rescue LoadError, Bundler::GemNotFound => e
  warn e.message
  puts "SKIP: specification tests require the locked gems; run bundle install --gemfile=test/spec/Gemfile"
  exit 77
end

$LOAD_PATH.unshift(File.join(__dir__, "lib"))
