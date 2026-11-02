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
      IsoWriter.compare(expected: expected, actual: observed, context: "libarchive fixture", multi_sl: true)
    end
    assert_includes output, "EXCEPTION"
    wrong = { "/long-target" => observed.fetch("/long-target").merge("target" => "x" * 1019) }
    assert_raises(IsoWriter::Error) do
      IsoWriter.compare(expected: expected, actual: wrong, context: "unrelated target corruption", multi_sl: true)
    end
    wrong_size = { "/long-target" => observed.fetch("/long-target").merge("size" => 1) }
    assert_raises(IsoWriter::Error) do
      IsoWriter.compare(expected: expected, actual: wrong_size, context: "unrelated size corruption", multi_sl: true)
    end
  end

  def test_link_listing_exception_does_not_interpret_placeholder_size_as_target_length
    expected = { "/long-target" => { "kind" => "symlink", "target" => IsoWriter::LONG_TARGET } }
    text = "lrwxrwxrwx 1 0 0 0 Nov 14 2023 '/long-target' -> '#{IsoWriter::LIBARCHIVE_LONG_TARGET}'\n"
    listed = IsoWriter.xorriso_listing(text)
    assert_equal IsoWriter::LIBARCHIVE_LONG_TARGET.bytesize, listed.fetch("/long-target").fetch("size")
    output, = capture_io do
      IsoWriter.compare(expected: expected, actual: listed, context: "link listing", multi_sl: true)
    end
    assert_includes output, "EXCEPTION"
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
