# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "minitest/autorun"
require "fileutils"
require "open3"
require "rbconfig"
require "tmpdir"
require_relative "iso9660-writer"

class IsoWriterTest < Minitest::Test
  def test_consumer_text_commands_use_utf8_locale
    arguments = [RbConfig.ruby, "-e", 'STDOUT.write(ENV.fetch("LC_ALL") + ":" + ENV.fetch("TZ"))']
    assert_equal "C.UTF-8:UTC", IsoIntegration.command(*arguments)
    assert_equal "C.UTF-8:UTC", IsoWriter.command(*arguments)
  end

  def test_consumer_payload_uses_utf8_locale_without_transcoding_bytes
    code = 'abort "wrong locale" unless ENV["LC_ALL"] == "C.UTF-8" && ENV["TZ"] == "UTC"; STDOUT.binmode; STDOUT.write([255, 0, 195, 169].pack("C*"))'
    bytes = [255, 0, 195, 169].pack("C*")
    assert_equal({ "sha256" => Digest::SHA256.hexdigest(bytes), "size" => bytes.bytesize },
                 IsoWriter.digest(command: [RbConfig.ruby, "-e", code]))
  end

  def test_isoinfo_preserves_filename_spaces_with_each_fixed_delimiter
    [1, 2].each do |width|
      rows = [".", "..", " leading", "trailing ", " both "].map do |name|
        "---------- 1 0 0 1 Nov 14 2023 [ 40 00]#{' ' * width}#{name} \n"
      end
      listing = "Directory listing of /\n" + rows.join
      with_listing_program(listing) do |program|
        actual = IsoIntegration.isoinfo_tree("unused.iso", view: "joliet", program: program)
        assert_equal ["/ leading", "/trailing ", "/ both "], actual.keys
      end
    end
  end

  def test_isoinfo_preserves_utf8_names_in_joliet_and_rock_ridge
    listing = "Directory listing of /\n" + [".", "..", "caf\u00e9.txt", "\u20ac.txt"].map do |name|
      "---------- 1 0 0 1 Nov 14 2023 [ 40 00]  #{name} \n"
    end.join
    with_listing_program(listing) do |program|
      %w[joliet rockridge].each do |view|
        actual = IsoIntegration.isoinfo_tree("unused.iso", view: view, program: program)
        assert_equal ["/caf\u00e9.txt", "/\u20ac.txt"], actual.keys
      end
    end
  end

  def test_isoinfo_preserves_empty_and_space_terminated_symlink_targets
    listing = "Directory listing of /\n" +
      "drwxr-xr-x 1 0 0 2048 Nov 14 2023 [ 40 02]  . \n" +
      "lrwxrwxrwx 1 0 0 0 Nov 14 2023 [ 0 00]  root -> \n" +
      "lrwxrwxrwx 1 0 0 0 Nov 14 2023 [ 0 00]  spaced -> target \n"
    with_listing_program(listing) do |program|
      rows = IsoIntegration.isoinfo_tree("unused.iso", view: "rockridge", program: program)
      assert_equal "", rows.fetch("/root").fetch(:target)
      assert_equal "target ", rows.fetch("/spaced").fetch(:target)
    end
  end

  def test_libarchive_synthesized_parent_has_no_stored_directory_metadata
    Dir.mktmpdir("iso-synthesized-parent-") do |scratch|
      skip "whole-tree extraction requires a case-sensitive filesystem" unless IsoWriter.case_sensitive?(directory: scratch)

      program = File.join(scratch, "bsdtar")
      File.write(program, <<~RUBY)
        #!#{RbConfig.ruby}
        # SPDX-License-Identifier: BSD-2-Clause
        # Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
        if ARGV.include?("-tv")
          puts "-rw-r--r-- 1 0 0 1 Nov 14 2023 rr_moved/member" unless ARGV.include?("wrongkind.iso")
        else
          root = ARGV.fetch(ARGV.index("-C") + 1)
          if ARGV.include?("wrongkind.iso")
            File.binwrite(File.join(root, "rr_moved"), "")
          else
            Dir.mkdir(File.join(root, "rr_moved"))
            member = File.join(root, "rr_moved/member")
            File.binwrite(member, "x")
            File.utime(1700000000, 1700000000, member)
          end
        end
      RUBY
      File.chmod(0o700, program)
      expected = { "/rr_moved" => { "kind" => "directory", "size" => 0, "modified" => 1700000000 },
                   "/rr_moved/member" => { "kind" => "file", "size" => 1, "modified" => 1700000000,
                                           "uid" => 0, "gid" => 0, "mode" => 0o644,
                                           "sha256" => Digest::SHA256.hexdigest("x") } }
      fixture = IsoWriter::Fixture.new(profile: :nonempty_container, level: 3)
      output, = capture_io do
        IsoWriter.bsdtar(image: "unused.iso", view: "rockridge", expected: expected,
                         program: program, scratch: scratch, fixture: fixture)
      end
      assert_includes output, "EXCEPTION"
      wrongkind = File.join(scratch, "wrongkind")
      Dir.mkdir(wrongkind)
      error = assert_raises(IsoWriter::Error) do
        capture_io do
          IsoWriter.bsdtar(image: "wrongkind.iso", view: "rockridge", expected: expected.slice("/rr_moved"),
                           program: program, scratch: wrongkind, fixture: fixture)
        end
      end
      assert_includes error.message, "kind differs"
    end
  end

  def test_xorriso_reports_only_the_exact_internal_empty_component_exception
    listing = "lrwxrwxrwx 1 0 0 0 Nov 14 2023 '/odd' -> 'a/b/./../c/'\n"
    expected = { "/odd" => { "kind" => "symlink", "target" => "a//b/./../c/",
                             "mode" => 0o777, "uid" => 0, "gid" => 0, "modified" => 1700000000 } }
    with_listing_program(listing) do |program|
      output, = capture_io { IsoWriter.xorriso(image: "unused.iso", expected: expected, program: program) }
      assert_includes output, "EXCEPTION"
    end
  end

  def test_text_commands_decode_utf8_with_ascii_default_encoding
    script = <<~'RUBY'
      require ARGV.fetch(0)
      listing = "-rw-r--r-- 1 0 0 1 Nov 14 2023 '/caf\u00e9.txt'\n"
      arguments = [RbConfig.ruby, "-e", "STDOUT.write(#{listing.dump})"]
      [IsoIntegration.command(*arguments), IsoWriter.command(*arguments)].each do |output|
        rows = IsoWriter.xorriso_listing(output)
        abort "wrong path" unless rows.keys == ["/caf\u00e9.txt"]
      end
    RUBY
    library = File.expand_path("iso9660-writer", __dir__)
    _output, errors, status = Open3.capture3(RbConfig.ruby, "-E", "US-ASCII", "-e", script, library)
    assert status.success?, errors
  end

  def test_utf8_text_interpretation_preserves_source_bytes_and_encoding
    output = "/caf\u00e9.txt".dup.force_encoding(Encoding::US_ASCII).freeze
    bytes = output.bytes

    assert_equal "/caf\u00e9.txt", IsoIntegration.utf8_text(output)
    assert_equal Encoding::US_ASCII, output.encoding
    assert_equal bytes, output.bytes
  end

  def test_ndjson_names_decode_as_utf8_independently_of_capture_encoding
    output = "{\"path\":\"/caf\u00e9.txt\"}\n".dup.force_encoding(Encoding::US_ASCII).freeze
    bytes = output.bytes

    assert_equal [{ "path" => "/caf\u00e9.txt" }], IsoIntegration.records(output)
    assert_equal Encoding::US_ASCII, output.encoding
    assert_equal bytes, output.bytes
  end

  def test_ndjson_decoder_refuses_malformed_json
    assert_raises(JSON::ParserError) { IsoIntegration.records("{\"path\":\n") }
  end

  def test_successful_xorriso_load_does_not_hide_wrong_names
    Dir.mktmpdir("iso-xorriso-test-") do |directory|
      program = File.join(directory, "xorriso")
      File.write(program, <<~SH)
        #!/bin/sh
        # SPDX-License-Identifier: BSD-2-Clause
        # Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
        printf '%s\n' "-rw-r--r-- 1 0 0 1 Nov 14 2023 '/wrong'"
      SH
      File.chmod(0o700, program)
      expected = { "/expected" => { "kind" => "file", "size" => 1, "mode" => 0o644,
                                     "uid" => 0, "gid" => 0, "modified" => 1700000000 } }
      assert_raises(IsoWriter::Error) { IsoWriter.xorriso(image: "unused.iso", expected: expected, program: program) }
    end
  end

  def test_primary_adapter_removes_only_extensionless_file_dot
    rows = { "/FOO." => { "kind" => "file" }, "/DIR" => { "kind" => "directory" } }
    projected = IsoWriter.libarchive_primary_projection(rows: rows)
    assert_equal %w[/FOO /DIR], projected.keys
  end

  def test_primary_adapter_refuses_file_directory_name_collapse
    rows = { "/FOO." => { "kind" => "file" }, "/FOO" => { "kind" => "directory" } }
    assert_raises(IsoWriter::Error) { IsoWriter.libarchive_primary_projection(rows: rows) }
  end

  def test_joliet_adapter_compares_nfc_and_nfd_as_equivalent
    entry = { "kind" => "file", "size" => 1 }.freeze
    expected = { "/caf\u00e9.txt" => entry }
    observed = { "/cafe\u0301.txt" => entry }
    assert_equal IsoWriter.libarchive_joliet_projection(rows: expected),
      IsoWriter.libarchive_joliet_projection(rows: observed)
  end

  def test_joliet_adapter_rejects_collapsed_distinct_names_in_either_set
    distinct = { "/caf\u00e9.txt" => { "kind" => "file", "size" => 1 },
                 "/cafe\u0301.txt" => { "kind" => "file", "size" => 2 } }
    assert_raises(IsoWriter::Error) { IsoWriter.libarchive_joliet_projection(rows: distinct) }
    assert_raises(IsoWriter::Error) { IsoWriter.libarchive_joliet_projection(rows: distinct.to_a.reverse.to_h) }
  end

  def test_libarchive_link_exception_requires_the_exact_separator_loss
    expected = { "/long-target" => { "kind" => "symlink", "size" => IsoWriter::LONG_TARGET.bytesize,
                                     "target" => IsoWriter::LONG_TARGET } }
    observed = { "/long-target" => { "kind" => "symlink", "size" => IsoWriter::LIBARCHIVE_LONG_TARGET.bytesize,
                                     "target" => IsoWriter::LIBARCHIVE_LONG_TARGET } }
    output, = capture_io do
      IsoWriter.compare(expected: expected, actual: observed, context: "libarchive fixture",
                        target_exceptions: IsoWriter::LIBARCHIVE_LINK_EXCEPTIONS)
    end
    assert_includes output, "EXCEPTION"
    wrong = { "/long-target" => observed.fetch("/long-target").merge("target" => "x" * 1019) }
    assert_raises(IsoWriter::Error) do
      IsoWriter.compare(expected: expected, actual: wrong, context: "unrelated target corruption",
                        target_exceptions: IsoWriter::LIBARCHIVE_LINK_EXCEPTIONS)
    end
    wrong_size = { "/long-target" => observed.fetch("/long-target").merge("size" => 1) }
    assert_raises(IsoWriter::Error) do
      IsoWriter.compare(expected: expected, actual: wrong_size, context: "unrelated size corruption",
                        target_exceptions: IsoWriter::LIBARCHIVE_LINK_EXCEPTIONS)
    end
  end

  def test_link_listing_exception_does_not_interpret_placeholder_size_as_target_length
    expected = { "/long-target" => { "kind" => "symlink", "target" => IsoWriter::LONG_TARGET } }
    text = "lrwxrwxrwx 1 0 0 0 Nov 14 2023 '/long-target' -> '#{IsoWriter::LIBARCHIVE_LONG_TARGET}'\n"
    listed = IsoWriter.xorriso_listing(text)
    assert_equal IsoWriter::LIBARCHIVE_LONG_TARGET.bytesize, listed.fetch("/long-target").fetch("size")
    output, = capture_io do
      IsoWriter.compare(expected: expected, actual: listed, context: "link listing",
                        target_exceptions: IsoWriter::XORRISO_LINK_EXCEPTIONS)
    end
    assert_includes output, "EXCEPTION"
  end

  def test_link_exceptions_require_exact_path_targets_sizes_and_symlink_kinds
    source = { "kind" => "symlink", "size" => 12, "target" => "a//b/./../c/" }
    measured = { "kind" => "symlink", "size" => 11, "target" => "a/b/./../c/" }
    expected = { "/odd" => source }
    actual = { "/odd" => measured }
    assert_raises(IsoWriter::Error) { IsoWriter.compare(expected: expected, actual: actual, context: "strict default") }
    assert_raises(IsoWriter::Error) do
      IsoWriter.compare(expected: expected, actual: actual, context: "other consumer",
                        target_exceptions: IsoWriter::LIBARCHIVE_LINK_EXCEPTIONS)
    end
    capture_io do
      IsoWriter.compare(expected: expected, actual: actual, context: "measured xorriso",
                        target_exceptions: IsoWriter::XORRISO_LINK_EXCEPTIONS)
    end
    [measured.merge("size" => 10), measured.merge("target" => "x" * 11),
     measured.merge("kind" => "file")].each do |wrong|
      assert_raises(IsoWriter::Error) do
        IsoWriter.compare(expected: expected, actual: { "/odd" => wrong }, context: "other corruption",
                          target_exceptions: IsoWriter::XORRISO_LINK_EXCEPTIONS)
      end
    end
    [source.merge("size" => 13), source.merge("kind" => "file")].each do |wrong|
      assert_raises(IsoWriter::Error) do
        IsoWriter.compare(expected: { "/odd" => wrong }, actual: actual, context: "inconsistent source",
                          target_exceptions: IsoWriter::XORRISO_LINK_EXCEPTIONS)
      end
    end
    assert_raises(IsoWriter::Error) do
      IsoWriter.compare(expected: { "/other" => source }, actual: { "/other" => measured }, context: "other path",
                        target_exceptions: IsoWriter::XORRISO_LINK_EXCEPTIONS)
    end
  end

  def test_directory_entry_exception_never_covers_descendants
    empty = { "/rr_moved" => { "kind" => "directory", "size" => 0 } }
    capture_io do
      IsoWriter.compare(expected: empty, actual: {}, context: "empty source directory", omitted_directory: "/rr_moved")
    end
    populated = empty.merge("/rr_moved/member" => { "kind" => "file", "size" => 1 })
    assert_raises(IsoWriter::Error) do
      IsoWriter.compare(expected: populated, actual: {}, context: "populated source directory",
                        omitted_directory: "/rr_moved")
    end
    capture_io do
      IsoWriter.compare(expected: populated, actual: populated.reject { |path, _entry| path == "/rr_moved" },
                        context: "directory omitted with child retained", omitted_directory: "/rr_moved")
    end
  end

  def test_extraction_comparison_checks_modification_time
    expected = { "/file" => { "kind" => "file", "size" => 1, "modified" => 1000 } }
    actual = { "/file" => expected.fetch("/file").merge("modified" => 1001) }
    assert_raises(IsoWriter::Error) do
      IsoWriter.compare(expected: expected, actual: actual, context: "extraction", metadata: false)
    end
  end

  def test_mount_that_acquires_then_reports_failure_is_unmounted
    with_fake_mount(mount_status: 1, unmount_status: 0) do |files, result|
      _output, errors, status = result
      refute status.success?
      assert_equal %w[mount umount], File.readlines(files.fetch(:calls), chomp: true)
      assert_includes errors, "mount"
      refute File.exist?(File.read(files.fetch(:root)).strip), "owned temporary mount root must be removed"
    end
  end

  def test_cleanup_failure_retains_primary_error_and_names_retained_resource
    with_fake_mount(mount_status: 0, unmount_status: 1) do |files, result|
      _output, errors, status = result
      refute status.success?
      assert_includes errors, "primary operation failed"
      assert_includes errors, "umount"
      root = File.read(files.fetch(:root)).strip
      assert_includes errors, root
      assert File.file?(File.join(root, "held")), "cleanup must not recurse through a retained mount"
    end
  end

  private

  def with_listing_program(output)
    Dir.mktmpdir("iso-listing-test-") do |directory|
      program = File.join(directory, "listing")
      File.write(program, <<~RUBY)
        #!#{RbConfig.ruby}
        # SPDX-License-Identifier: BSD-2-Clause
        # Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
        STDOUT.binmode
        STDOUT.write(#{output.b.dump})
      RUBY
      File.chmod(0o700, program)
      yield program
    end
  end

  # Only fake executables receive these calls. The child's TMPDIR is also
  # beneath the disposable fixture, so a seeded cleanup failure leaves no
  # resource outside it and never performs an actual mount.
  def with_fake_mount(mount_status:, unmount_status:)
    Dir.mktmpdir("iso-writer-test-") do |directory|
      programs = File.join(directory, "bin")
      Dir.mkdir(programs)
      files = { calls: File.join(directory, "calls"), root: File.join(directory, "root") }
      File.write(File.join(programs, "mount"), <<~SH)
        #!/bin/sh
        # SPDX-License-Identifier: BSD-2-Clause
        # Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
        for mount_root do :; done
        printf '%s\n' "$mount_root" > "$FSIMG_TEST_ROOT"
        printf 'mount\n' >> "$FSIMG_TEST_CALLS"
        /usr/bin/touch "$mount_root/held"
        exit #{mount_status}
      SH
      File.write(File.join(programs, "umount"), <<~SH)
        #!/bin/sh
        # SPDX-License-Identifier: BSD-2-Clause
        # Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
        printf 'umount\n' >> "$FSIMG_TEST_CALLS"
        if test #{unmount_status} -eq 0; then /bin/rm "$1/held"; fi
        exit #{unmount_status}
      SH
      %w[mount umount].each { File.chmod(0o700, File.join(programs, _1)) }
      runner = File.expand_path("iso9660-kernel.rb", __dir__)
      code = <<~RUBY
        begin
          IsoKernel.with_mount(image: "unused.iso", view: "rockridge", platform: "Linux") do
            raise IsoWriter::Error, "primary operation failed"
          end
        rescue StandardError => error
          warn error.message
          exit 1
        end
      RUBY
      environment = { "PATH" => "#{programs}:/usr/bin:/bin", "TMPDIR" => directory,
                      "FSIMG_TEST_CALLS" => files.fetch(:calls), "FSIMG_TEST_ROOT" => files.fetch(:root) }
      yield files, Open3.capture3(environment, RbConfig.ruby, "-r", runner, "-e", code)
    end
  end
end
