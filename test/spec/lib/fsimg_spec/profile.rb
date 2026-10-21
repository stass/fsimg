# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # The bounds a profile places on every case: the image's logical size, the
  # library memory budget fsimg gets (-m), and one value per fsimg -l
  # resource. Generators and the recipe loader keep within them, and the
  # runner passes them to fsimg. path stays well below macOS's 1024-byte
  # PATH_MAX once the case directory's own path is added.
  Ceilings = Data.define(:image, :memory, :scratch, :entries, :depth, :path, :fds, :output, :work, :seconds) do
    def limit(resource) = to_h.fetch(resource)
  end

  # A named run configuration (specs/design-spec-testing.md#profiles): its
  # ceilings, the number of random trees a run draws and classifies, the
  # sample of them, the first tree_sample, that runs fsimg, and the number
  # of trees drawn for mtools to populate, every one of which runs fsimg.
  # Each profile writes its report into its own results directory under
  # the build directory, so runs of different profiles never share one.
  Profile = Data.define(:name, :ceilings, :tree_draws, :tree_sample, :populated_trees) do
    def results_directory(build_directory) =
      File.join(build_directory, "test", "spec-results", name)
  end

  MIB = 1 << 20

  # quick's output ceiling admits the FAT16 and FAT32 threshold volumes of
  # 4096-byte clusters, about 257 MiB each, which create writes sparse.
  PROFILES = [
    Profile.new(name: "quick", ceilings: Ceilings.new(
      image: 1024 * MIB, memory: 64 * MIB, scratch: 256 * MIB, entries: 10_000, depth: 32,
      path: 512, fds: 128, output: 512 * MIB, work: 1 << 40, seconds: 20
    ), tree_draws: 150, tree_sample: 15, populated_trees: 6),
    Profile.new(name: "nightly", ceilings: Ceilings.new(
      image: 4096 * MIB, memory: 64 * MIB, scratch: 1024 * MIB, entries: 100_000, depth: 64,
      path: 512, fds: 128, output: 1024 * MIB, work: 1 << 40, seconds: 60
    ), tree_draws: 3000, tree_sample: 300, populated_trees: 150),
    Profile.new(name: "deep", ceilings: Ceilings.new(
      image: 65_536 * MIB, memory: 64 * MIB, scratch: 8192 * MIB, entries: 1_000_000, depth: 96,
      path: 512, fds: 160, output: 8192 * MIB, work: 1 << 40, seconds: 600
    ), tree_draws: 30_000, tree_sample: 3000, populated_trees: 1500)
  ].to_h { [_1.name, _1] }.freeze

  def self.fetch_profile(name) =
    PROFILES.fetch(name) do
      raise UsageError, "unknown profile #{name.inspect}; expected one of #{PROFILES.keys.join(", ")}"
    end
end
