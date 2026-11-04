# frozen_string_literal: true

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# What experiment E9 puts on a mounted volume: a catalog large enough for
# several index levels, and the deletions that follow.
#
#   d00 ... d49/file-NNNNN[x...]   DIRECTORIES x FILES_PER_DIRECTORY empty files;
#                                  every seventh name carries one to six extra
#                                  characters, so key lengths vary
#
# e9_create makes the tree. e9_delete, run on a volume e9_create made, removes
# every third file of every directory, all of d07 and the files 500 to 1499 of
# d20, so that scattered and contiguous runs of catalog records disappear. Each
# step observes what it did. e9_thin, run on a volume e9_delete left, removes
# every file whose number is not a multiple of THIN_EVERY, so that most leaf
# nodes hold one record or none; e9_sparse does the same with SPARSE_EVERY, a
# prime, so that few leaf nodes of any index node survive.
require "fileutils"
require_relative "populate"

module HfsPlusPopulateLarge
  P = HfsPlusPopulate
  DIRECTORIES = 50
  FILES_PER_DIRECTORY = 2000
  WIPED_DIRECTORY = 7
  RUN_DIRECTORY = 20
  RUN_FILES = (500...1500).freeze
  THIN_EVERY = 50
  SPARSE_EVERY = 997

  module_function

  def e9_create = [P.step("create", "d00/file-00000") { |root| create(root) }]

  def e9_delete = [P.step("delete", "d00") { |root| delete(root) }]

  def e9_thin = [P.step("thin", "d00") { |root| thin(root, THIN_EVERY) }]

  def e9_sparse = [P.step("sparse", "d00") { |root| thin(root, SPARSE_EVERY) }]

  def dir_name(number) = format("d%02d", number)

  # The name carries `index % 7` extra characters, none for six sevenths.
  def file_name(index) = format("file-%05d", index) + ("x" * (index % 7 == 0 ? index % 6 + 1 : 0))

  def create(root)
    DIRECTORIES.times do |d|
      dir = File.join(root, dir_name(d))
      Dir.mkdir(dir)
      FILES_PER_DIRECTORY.times { File.write(File.join(dir, file_name(_1)), "") }
    end
    P.observe("created #{DIRECTORIES * FILES_PER_DIRECTORY} files in #{DIRECTORIES} directories")
  end

  def delete(root)
    doomed = (0...DIRECTORIES).flat_map do |d|
      indexes = d == WIPED_DIRECTORY ? (0...FILES_PER_DIRECTORY).to_a : (0...FILES_PER_DIRECTORY).select { _1 % 3 == 0 || (d == RUN_DIRECTORY && RUN_FILES.cover?(_1)) }
      indexes.map { File.join(root, dir_name(d), file_name(_1)) }
    end
    doomed.each { File.delete(_1) }
    P.observe("deleted #{doomed.size} files")
  end

  # The file number is the five digits after "file-".
  def thin(root, every)
    doomed = Dir.glob(File.join(root, "d*", "file-*")).reject { File.basename(_1)[5, 5].to_i % every == 0 }
    doomed.each { File.delete(_1) }
    P.observe("deleted #{doomed.size} files")
  end
end
