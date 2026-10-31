# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "fileutils"
require "securerandom"

module FsimgSpec
  module Fat
    # Fuzz seeds from the FAT cases (specs/design-spec-testing.md#profiles):
    # the image of every targeted witness and every promoted case, for the
    # native FAT fuzzer to start from. The fuzzer detects the format itself,
    # so a case that forces one with -t, which the fuzzer cannot express,
    # is not seeded.
    #
    # The fuzzer reads at most MAX_BYTES, build-aux/fuzz.sh's input ceiling,
    # as an exact-length image and again as the prefix of zero-filled
    # logical storage (fuzz/fat.c). An image that fits is its own seed,
    # length and all, since some cases test the length itself. A larger
    # image is cut after its last byte that is not zero, which the second
    # reading restores, though as part of a larger volume; one whose data
    # passes the ceiling cannot be a seed, and the export fails rather than
    # seed a different image.
    module Seeds
      MAX_BYTES = 3 << 20
      PREFIX = "spec-"
      SUFFIX = ".seed"
      SCAN_BYTES = 1 << 20

      # Writes the seed of each classified case into directory, replacing a
      # seed of the same name, and answers the names written. Every other
      # file named like a seed, spec-*.seed, is taken for one an earlier
      # export wrote and removed. A seed is named after its case, whose id
      # the recipe format keeps to a file name.
      def self.export(directory, cases)
        names = cases.map do |classified|
          name = "#{PREFIX}#{classified.kase.recipe.id}#{SUFFIX}"
          replace(File.join(directory, name), seed(classified))
          name
        end
        seeds = Dir.children(directory).select { _1.start_with?(PREFIX) && _1.end_with?(SUFFIX) }
        (seeds - names).each { FileUtils.rm_f(File.join(directory, _1)) }
        names
      end

      # Every case of the FAT suite that seeds the fuzzer, classified. A
      # case the host skips has no image to seed from; one that cannot be
      # classified fails the export, as it fails a run. A case whose
      # generator was wrong or whose promoted recipe drifted still has an
      # image, which seeds the fuzzer as well as any. A seed needs the
      # image, not the host's answers for its names, so no name trial runs.
      def self.seeded(cache:, ceilings:) =
        Suite.cases(cache:, ceilings:, name_trials: NameTrials::NONE, shell: nil, draws: Draws::NONE).reject(&:skip)
             .map { CasePhase.classify(_1) }.select { seeds?(_1.kase.recipe) }

      # Whether recipe's case seeds the fuzzer: a targeted or promoted
      # reading case whose format the fuzzer detects itself. A creation
      # case has no image until fsimg writes one.
      def self.seeds?(recipe) = recipe.reading? && (recipe.kind == "targeted" || recipe.promoted?) && recipe.forced_format.nil?

      # The case's image when it fits the fuzzer's input, else its image up
      # to its last byte that is not zero.
      def self.seed(classified)
        image = classified.image
        return image.read(0, image.length) if image.length <= MAX_BYTES

        length = content_length(image)
        if length > MAX_BYTES
          raise HarnessError, "#{classified.kase.recipe.id}: its image holds data at #{length - 1}, " \
                              "past the fuzzer's #{MAX_BYTES}-byte input"
        end

        image.read(0, length)
      end

      # The length of image without its trailing zero bytes, found by
      # scanning back from its end SCAN_BYTES at a time. Counting zeros,
      # which runs in C, passes over an all-zero chunk without a search.
      def self.content_length(image)
        stop = image.length
        while stop.positive?
          start = [stop - SCAN_BYTES, 0].max
          bytes = image.read(start, stop - start)
          last = bytes.rindex(/[^\0]/n) unless bytes.count("\0") == bytes.bytesize
          return start + last + 1 if last

          stop = start
        end
        0
      end

      # Writes bytes to a fresh file beside path and renames it over path,
      # so a reader of the corpus never sees half a seed. The temporary name
      # is random and created exclusively, so nothing already at it, a link
      # or another export's file, is written through; once created, it is
      # removed unless it is renamed, whatever ends the write or the
      # rename, an interrupt included. rename replaces a link at path
      # without following it. An export killed by SIGKILL, or interrupted
      # just as the open returns, leaves its temporary file, a partial or
      # whole seed under another name, until the corpus is removed; the
      # fuzzer takes it for one more input.
      private_class_method def self.replace(path, bytes)
        temporary = "#{path}.#{SecureRandom.hex(8)}.tmp"
        created = renamed = false
        File.open(temporary, File::WRONLY | File::CREAT | File::EXCL | File::NOFOLLOW | File::BINARY, 0o600) do |out|
          created = true
          out.write(bytes)
        end
        File.rename(temporary, path)
        renamed = true
      ensure
        FileUtils.rm_f(temporary) if created && !renamed
      end
    end
  end
end
