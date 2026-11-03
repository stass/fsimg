# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require 'minitest/autorun'
require 'tmpdir'
require 'rbconfig'
require_relative 'iso9660-corpus'
require_relative 'iso9660-evidence'
require_relative 'iso9660-integration'

class IsoCorpusTest < Minitest::Test
  def test_device_manifest_absence_raises_integration_error
    Dir.mktmpdir('iso-devices-') do |directory|
      image = File.join(directory, 'devices.iso')
      File.write(image.delete_suffix('.iso') + '.rockridge.jsonl',
                 JSON.generate({ path: '/dev/null', type: 'characterSpecial' }) + "\n")
      error = assert_raises(IsoIntegration::Error) do
        IsoIntegration.compare_rr_manifest(binary: 'unused', options: [], image: image,
                                           actual: { '/dev/null' => { kind: 'char_device' } })
      end
      assert_equal 'device manifest absent', error.message
    end
  end

  def test_pn_words_preserve_both_halves_of_the_packed_device_number
    assert_equal({ dev_major: 0, dev_minor: 0x03000002 }, IsoCorpus.pn_words(0x03000002))
    assert_equal({ dev_major: 0x12345678, dev_minor: 0x9abcdef0 },
                 IsoCorpus.pn_words(0x123456789abcdef0))
    assert_equal({ dev_major: 0xffffffff, dev_minor: 0xffffffff },
                 IsoCorpus.pn_words(0xffffffffffffffff))
  end

  def test_xorriso_device_disc_lists_device_words_and_extracts_only_ordinary_entries
    program = IsoCorpus.executable('xorriso')
    skip('xorriso and isoinfo required for device corpus') unless program && IsoCorpus.executable('isoinfo')
    binary = File.expand_path('fsimg')
    skip('run in the build directory after make to compare the device corpus with fsimg') unless File.executable?(binary)

    Dir.mktmpdir('iso-devices-') do |directory|
      corpus = IsoCorpus::Corpus.new(directory)
      words = corpus.device_disc(program: program, version: 'test', sources: File.join(directory, 'sources'))
      image = File.join(directory, 'xorriso-devices.iso')
      oracle = File.readlines(image.delete_suffix('.iso') + '.rockridge.jsonl').map { JSON.parse(_1) }
      kinds = { 'file' => 'file', 'directory' => 'directory', 'socket' => 'socket',
                'characterSpecial' => 'char_device', 'blockSpecial' => 'block_device' }
      expected = oracle.to_h { [_1.fetch('path'), kinds.fetch(_1.fetch('type'))] }
      assert_equal 'char_device', expected.fetch('/dev/null')
      assert_equal 'block_device', expected.fetch('/dev/block') if words.key?('/dev/block')
      assert_equal 'socket', expected.fetch('/dev/socket') if File.socket?(File.join(directory, 'sources/devices/dev/socket'))
      assert_equal 'file', expected.fetch('/file')
      entries = IsoCorpus.capture(binary, 't', '--json', '-f', image).lines.map { JSON.parse(_1) }
        .select { _1.fetch('type') == 'entry' }.to_h { [_1.fetch('path'), _1] }
      assert_equal expected, entries.transform_values { _1.fetch('kind') }
      words.each do |path, fields|
        fields.each { |field, value| assert_equal value, entries.fetch(path).fetch(field.to_s), "#{path} #{field}" }
      end

      destination = File.join(directory, 'extract')
      Dir.mkdir(destination)
      output = IsoCorpus.capture(binary, 'x', '-k', '--json', '-f', image, '-C', destination)
      summary = output.lines.map { JSON.parse(_1) }.last
      assert_equal expected.values.count { %w[char_device block_device socket].include?(_1) },
                   Integer(summary.fetch('skipped_entries'), 10)
      extracted = IsoCorpus.source_rows(destination)
      assert extracted.all? { %w[directory file].include?(_1.fetch(:type)) }
      files = extracted.select { _1.fetch(:type) == 'file' }
      assert_equal 1, files.size
      assert_equal Digest::SHA256.hexdigest("device corpus\n"), files.first.fetch(:sha256)
    end
  end

  def test_cli_gives_children_private_working_and_temporary_directories_without_cores
    Dir.mktmpdir('iso-corpus-') do |directory|
      destination = File.join(directory, 'output')
      helper = File.expand_path('iso9660-corpus.rb', __dir__)
      program = <<~CODE
        IsoCorpus.prepare_output(ARGV.first)
        puts JSON.generate([Dir.pwd, ENV.fetch('TMPDIR'), Process.getrlimit(Process::RLIMIT_CORE)])
      CODE
      output, status = Open3.capture2e(RbConfig.ruby, '-r', helper, '-e', program, destination)
      assert status.success?, output
      working, temporary, core = JSON.parse(output)
      assert_equal File.realpath(destination), working
      assert_equal File.join(working, 'tmp'), temporary
      assert File.directory?(temporary)
      assert_equal [0, 0], core
      assert_equal 0o700, File.stat(working).mode & 0o777
    end
  end

  def test_cli_refuses_a_symlink_alias_into_the_repository
    Dir.mktmpdir('iso-corpus-') do |directory|
      repository = File.realpath(File.join(__dir__, '..'))
      link = File.join(directory, 'alias')
      File.symlink(repository, link)
      destination = File.join(link, 'forbidden-corpus')
      helper = File.expand_path('iso9660-corpus.rb', __dir__)
      output, status = Open3.capture2e(RbConfig.ruby, '-r', helper, '-e',
                                       'IsoCorpus.prepare_output(ARGV.first)', destination)
      refute status.success?
      assert_includes output, 'output must be outside the repository'
      refute File.exist?(destination)
    end
  end

  def test_content_read_drains_large_stderr_and_returns_only_stdout_hash
    program = '$stdout.write("x"); $stderr.write("diagnostic" * 20000)'
    digest, size = IsoCorpus.content_digest([RbConfig.ruby, '-e', program])
    assert_equal 1, size
    assert_equal Digest::SHA256.hexdigest('x'), digest
  end

  def test_single_section_extraction_failure_does_not_become_a_source_hash
    Dir.mktmpdir('iso-corpus-') do |directory|
      tree = File.join(directory, 'minimal')
      Dir.mkdir(tree)
      File.write(File.join(tree, 'ONE.TXT'), "one\n")
      # An oracle that lists a four-byte file but extracts no bytes.
      File.write(File.join(directory, 'isoinfo'), <<~SH)
        #!/bin/sh
        # SPDX-License-Identifier: BSD-2-Clause
        # Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
        case "$1" in
        -l) printf 'Directory listing of /\\n-rw-r--r-- 1 0 0 4 Nov 1 2026 [ 32 00] ONE.TXT;1\\n' ;;
        -f) printf '/ONE.TXT;1\\n' ;;
        esac
      SH
      File.chmod(0o700, File.join(directory, 'isoinfo'))
      helper = File.expand_path('iso9660-corpus.rb', __dir__)
      program = <<~CODE
        corpus = IsoCorpus::Corpus.new(ARGV.first)
        corpus.view_manifest(image: File.join(ARGV.first, 'minimal.iso'), view: 'primary', flags: [],
                             source: IsoCorpus.source_rows(File.join(ARGV.first, 'minimal')))
      CODE
      output, status = Open3.capture2e({ 'PATH' => directory }, RbConfig.ruby,
                                       '-r', helper, '-e', program, directory)
      refute status.success?, 'incomplete single-section extraction must fail'
      assert_includes output, 'unexpected oracle extraction size'
    end
  end

  def test_xorriso_disables_rc_before_emulation
    row = IsoCorpus.invocations('xorriso', '/sources').first
    assert_equal ['-no_rc', '-as', 'mkisofs'], row.options.first(3)
  end

  def test_dialect_comes_from_version
    assert_equal 'genisoimage', IsoCorpus.dialect('mkisofs 1.1.11 (Linux) genisoimage')
    assert_equal 'mkisofs', IsoCorpus.dialect('mkisofs 3.02a09 (cdrtools)')
  end

  def test_content_failure_reports_command_and_exit_status
    error = assert_raises(IsoCorpus::ConsumerError) do
      IsoCorpus.content_digest([RbConfig.ruby, '-e', 'exit 9'])
    end
    assert_includes error.message, 'exit=9'
    assert_includes error.message, RbConfig.ruby
  end

  def test_content_signal_failure_reports_signal_status
    error = assert_raises(IsoCorpus::ConsumerError) do
      IsoCorpus.content_digest([RbConfig.ruby, '-e', 'Process.kill("KILL", Process.pid)'])
    end
    assert_includes error.message, "signal=#{Signal.list.fetch('KILL')}"
  end

  def test_evidence_compares_directory_identifiers_whole
    Dir.mktmpdir('iso-evidence-') do |directory|
      # Minimal Joliet metadata: root contains directories A-B and A.B in
      # whole-identifier order. Both endian path tables contain that order.
      record = lambda do |name:, extent:, size: 0|
        bytes = "\0".b * (33 + name.bytesize + (name.bytesize.even? ? 1 : 0))
        bytes.setbyte(0, bytes.bytesize)
        bytes[2, 8] = [extent, extent].pack('VN')
        bytes[10, 8] = [size, size].pack('VN')
        bytes.setbyte(25, 2)
        bytes.setbyte(32, name.bytesize)
        bytes[33, name.bytesize] = name
        bytes
      end
      tables = %w[Vv Nn].map do |formats|
        ["\0".b, 'A-B'.encode('UTF-16BE').b, 'A.B'.encode('UTF-16BE').b].zip([20, 21, 22]).map do |name, extent|
          [name.bytesize, 0].pack('CC') + [extent, 1].pack(formats) + name + "\0" * (name.bytesize % 2)
        end.join
      end
      image = "\0".b * (23 * 2048)
      descriptor = "\0".b * 2048
      descriptor[0, 7] = [2].pack('C') + 'CD001' + [1].pack('C')
      descriptor[88, 3] = '%/E'
      descriptor[132, 4] = [tables.first.bytesize].pack('V')
      descriptor[140, 4] = [18].pack('V')
      descriptor[148, 4] = [19].pack('N')
      descriptor[156, 34] = record.call(name: "\0".b, extent: 20, size: 2048)
      image[16 * 2048, 2048] = descriptor
      image.setbyte(17 * 2048, 255)
      tables.zip([18, 19]).each { |table, block| image[block * 2048, table.bytesize] = table }
      entries = [record.call(name: 'A-B'.encode('UTF-16BE').b, extent: 21),
                 record.call(name: 'A.B'.encode('UTF-16BE').b, extent: 22)].join
      image[20 * 2048, entries.bytesize] = entries
      path = File.join(directory, 'order.iso')
      File.binwrite(path, image)
      result = File.open(path, 'rb') { IsoEvidence.new(_1).call }
      assert_equal 0, result[:counts].fetch('joliet_directory_inversions')
      assert_equal 0, result[:counts].fetch('joliet_path_inversions_little')
      assert_equal 0, result[:counts].fetch('joliet_path_inversions_big')
    end
  end

  def test_producers_receive_private_home_without_host_rc_variables
    Dir.mktmpdir('iso-corpus-') do |directory|
      helper = File.expand_path('iso9660-corpus.rb', __dir__)
      program = <<~RUBY_CODE
        corpus = IsoCorpus::Corpus.new(ARGV.first)
        probe = 'puts ENV.values_at("HOME", "MKISOFSRC", "GENISOIMAGERC", "SOURCE_DATE_EPOCH").inspect'
        result = IsoCorpus.capture_result(RbConfig.ruby, '-e', probe,
          environment: corpus.producer_environment)
        puts result.stdout
        exit result.status.exitstatus
      RUBY_CODE
      inherited = { 'HOME' => '/host-home', 'MKISOFSRC' => '/host-mkisofsrc',
                    'GENISOIMAGERC' => '/host-genisoimagerc', 'SOURCE_DATE_EPOCH' => '1' }
      output, status = Open3.capture2e(inherited, RbConfig.ruby, '-r', helper, '-rrbconfig', '-e', program, directory)
      assert status.success?, output
      assert_equal [directory, nil, nil, '946684800'].inspect + "\n", output
    end
  end

  def test_executable_resolution_follows_symlinks
    Dir.mktmpdir('iso-corpus-') do |directory|
      program = File.join(directory, 'producer')
      File.write(program, '')
      File.chmod(0o700, program)
      File.symlink('producer', File.join(directory, 'mkisofs'))
      ruby = 'puts IsoCorpus.executable("mkisofs")'
      helper = File.expand_path('iso9660-corpus.rb', __dir__)
      output, status = Open3.capture2e({ 'PATH' => directory }, RbConfig.ruby, '-r', helper, '-e', ruby)
      assert status.success?, output
      assert_equal File.realpath(program), output.strip
    end
  end

  def test_timeout_terminates_a_tool_and_reports_failure
    error = assert_raises(IsoCorpus::ConsumerError) do
      IsoCorpus.subprocess([RbConfig.ruby, '-e', 'sleep 30'], seconds: 0.1) do |output, errors, child|
        output.read
        errors.read
        child.value
      end
    end
    assert_includes error.message, 'timed out'
  end

  def test_timeout_is_failure_when_a_successful_parent_leaves_pipes_open
    command = [RbConfig.ruby, '-e', 'fork { sleep 30 }; exit 0']
    error = assert_raises(IsoCorpus::ConsumerError) do
      IsoCorpus.subprocess(command, seconds: 0.5) do |output, errors, child|
        output.read
        errors.read
        child.value
      end
    end
    assert_includes error.message, 'timed out'
  end

  def test_consumer_failures_are_recorded_and_remaining_images_are_produced
    %w[-d -l -f -x].each do |failed_option|
      Dir.mktmpdir('iso-corpus-') do |directory|
        tree = File.join(directory, 'tree')
        Dir.mkdir(tree)
        File.write(File.join(tree, 'ONE.TXT'), "one\n")
        File.write(File.join(directory, 'isoinfo'), <<~SH)
          #!/bin/sh
          case " $* " in
          *" #{failed_option} "*) echo 'oracle failed'; exit 9 ;;
          esac
          case "$1" in
          -l) printf 'Directory listing of /\\n-rw-r--r-- 1 0 0 4 Nov 1 2026 [ 32 00] ONE.TXT;1\\n' ;;
          -f) printf '/ONE.TXT;1\\n' ;;
          *) printf 'one\\n' ;;
          esac
        SH
        File.chmod(0o700, File.join(directory, 'isoinfo'))
        helper = File.expand_path('iso9660-corpus.rb', __dir__)
        program = <<~CODE
          corpus = IsoCorpus::Corpus.new(ARGV.first)
          %w[first second].each do |name|
            corpus.record_image(name: name, tool: 'fake', version: '1',
              command: [RbConfig.ruby, '-e', '$stdout.write("iso")'], tree: File.join(ARGV.first, 'tree'))
          end
        CODE
        output, status = Open3.capture2e({ 'PATH' => directory }, RbConfig.ruby,
                                         '-r', helper, '-rrbconfig', '-e', program, directory)
        assert status.success?, output
        assert_equal 2, File.readlines(File.join(directory, 'images.jsonl')).size
        assert_includes File.read(File.join(directory, 'skips.txt')), 'consumer failed'
        assert_includes File.read(File.join(directory, 'second.iso.status')), 'consumer failed'
      end
    end
  end

  def test_complete_large_extraction_is_compared_before_acceptance
    Dir.mktmpdir('iso-corpus-') do |directory|
      # Fixture data identifies the expected source hash. The consumer can
      # successfully extract it without repeated -f paths or a basename rule.
      File.write(File.join(directory, 'isoinfo'), "#!/bin/sh\nprintf 'complete'\n")
      File.chmod(0o700, File.join(directory, 'isoinfo'))
      helper = File.expand_path('iso9660-corpus.rb', __dir__)
      program = <<~CODE
        rows = { '/BIG.BIN;1' => { path: '/BIG.BIN;1', type: 'file', size: 8 } }
        expected = { size: 8, sha256: Digest::SHA256.hexdigest('complete') }
        IsoCorpus.hash_view(image: 'fixture', flags: [], rows: rows,
          occurrences: { '/BIG.BIN;1' => 1 }, expected: expected)
        puts JSON.generate(rows.values.first)
      CODE
      output, status = Open3.capture2e({ 'PATH' => directory }, RbConfig.ruby, '-r', helper, '-e', program)
      assert status.success?, output
      row = JSON.parse(output)
      assert_equal row.fetch('sha256'), row.fetch('oracle_sha256')
      assert_equal 8, row.fetch('oracle_size')
    end
  end

  def test_short_large_listing_cannot_replace_the_fixture_hash
    Dir.mktmpdir('iso-corpus-') do |directory|
      File.write(File.join(directory, 'isoinfo'), "#!/bin/sh\nprintf 'suffix'\n")
      File.chmod(0o700, File.join(directory, 'isoinfo'))
      helper = File.expand_path('iso9660-corpus.rb', __dir__)
      program = <<~RUBY_CODE
        rows = { '/BIG.BIN;1' => { path: '/BIG.BIN;1', type: 'file', size: 6 } }
        expected = { size: (1 << 32) + 6, sha256: 'source digest' }
        IsoCorpus.hash_view(image: 'fixture', flags: [], rows: rows,
          occurrences: { '/BIG.BIN;1' => 1 }, expected: expected)
      RUBY_CODE
      output, status = Open3.capture2e({ 'PATH' => directory }, RbConfig.ruby, '-r', helper, '-e', program)
      refute status.success?, output
      assert_includes output, 'unexpected oracle listing size'
    end
  end

  def test_cli_prepares_sparse_fixtures_when_producers_are_missing
    Dir.mktmpdir('iso-corpus-') do |directory|
      destination = File.join(directory, 'output')
      helper = File.expand_path('iso9660-corpus.rb', __dir__)
      output, status = Open3.capture2e({ 'PATH' => directory }, RbConfig.ruby, helper, destination)
      assert status.success?, output
      assert_includes File.read(File.join(destination, 'skips.txt')), 'xorriso: missing'
      assert_equal (1 << 32) + 4100, File.size(File.join(destination, 'sources/large/BIG.BIN'))
      assert_equal 24, Dir.children(File.join(destination, 'sources/rr')).count { _1.end_with?('n' * 235) }
    end
  end

  def test_private_output_contains_empty_rc_files_that_stop_host_fallback
    Dir.mktmpdir('iso-corpus-') do |directory|
      destination = File.join(directory, 'output')
      helper = File.expand_path('iso9660-corpus.rb', __dir__)
      output, status = Open3.capture2e(RbConfig.ruby, '-r', helper, '-e',
                                       'IsoCorpus.prepare_output(ARGV.first)', destination)
      assert status.success?, output
      %w[.mkisofsrc .genisoimagerc].each do |name|
        assert_equal ''.b, File.binread(File.join(destination, name))
      end
    end
  end

  def test_successful_listing_warnings_do_not_become_member_paths
    Dir.mktmpdir('iso-corpus-') do |directory|
      File.write(File.join(directory, 'isoinfo'), <<~SH)
        #!/bin/sh
        echo 'oracle warning' >&2
        case "$1" in
        -l) printf 'Directory listing of /\\n-rw-r--r-- 1 0 0 4 Nov 1 2026 [ 32 00] ONE.TXT;1\\n' ;;
        -f) printf '/ONE.TXT;1\\n' ;;
        *) printf 'one\\n' ;;
        esac
      SH
      File.chmod(0o700, File.join(directory, 'isoinfo'))
      helper = File.expand_path('iso9660-corpus.rb', __dir__)
      program = <<~RUBY_CODE
        corpus = IsoCorpus::Corpus.new(ARGV.first)
        corpus.view_manifest(image: File.join(ARGV.first, 'image.iso'), view: 'primary', flags: [], source: [])
      RUBY_CODE
      output, status = Open3.capture2e({ 'PATH' => directory }, RbConfig.ruby, '-r', helper, '-e', program, directory)
      assert status.success?, output
      rows = File.readlines(File.join(directory, 'image.primary.jsonl')).map { JSON.parse(_1) }
      assert_equal ['/ONE.TXT;1'], rows.map { _1.fetch('path') }
      assert_includes File.read(File.join(directory, 'image.primary.paths.stderr')), 'oracle warning'
    end
  end

  def test_genisoimage_does_not_receive_cdrtools_rrip112_option
    rows = IsoCorpus.invocations('genisoimage', '/sources')
    refute rows.any? { _1.options.include?('-rrip112') }
  end

  def test_sparse_stream_preserves_content_trailing_holes_and_diagnostics
    Dir.mktmpdir('iso-corpus-') do |directory|
      image = File.join(directory, 'image')
      log = File.join(directory, 'log')
      program = '$stdout.binmode; $stdout.write("a" + "\0" * 2097152); warn "diagnostic"'
      digest = IsoCorpus.produce(command: [RbConfig.ruby, '-e', program], image: image, log: log)
      assert_equal Digest::SHA256.hexdigest("a" + "\0" * 2097152), digest
      assert_equal 2097153, File.size(image)
      assert_equal "diagnostic\n", File.read(log)
      assert_equal "a\0", File.binread(image, 2)
      assert_equal "\0", File.binread(image, 1, File.size(image) - 1)
    end
  end

  def test_failed_producer_reports_status
    Dir.mktmpdir('iso-corpus-') do |directory|
      error = assert_raises(IsoCorpus::ProducerError) do
        IsoCorpus.produce(command: [RbConfig.ruby, '-e', 'exit 7'],
                          image: File.join(directory, 'image'), log: File.join(directory, 'log'))
      end
      assert_match 'exit=7', error.message
    end
  end

  def test_content_hash_reports_actual_extracted_byte_count
    digest, size = IsoCorpus.content_digest([RbConfig.ruby, '-e', '$stdout.write("suffix")'])
    assert_equal 6, size
    assert_equal Digest::SHA256.hexdigest('suffix'), digest
  end

  def test_listing_accepts_both_prefixes_fifos_links_and_multiple_sections
    listing = <<~LISTING
      Directory listing of /
              30 drwxr-xr-x   2  501   20       2048 Nov  1 2026 [     30 02] directory
      frw-r--r--   1  501   20          0 Nov  1 2026 [     32 00] fifo
      crw-rw-rw-   1  501   20          0 Nov  1 2026 [     32 00] null
      brw-r-----   1  501   20          0 Nov  1 2026 [     32 00] block
      srwxr-xr-x   1  501   20          0 Nov  1 2026 [     32 00] socket
      lrwxr-xr-x   1  501   20          0 Nov  1 2026 [     32 00] link -> file
      -rw-r--r--   1  501   20       2048 Nov  1 2026 [     32 80] BIG.BIN;1
      -rw-r--r--   1  501   20          4 Nov  1 2026 [     33 00] BIG.BIN;1
    LISTING
    rows = IsoCorpus.decode_listing(listing)
    assert_equal 'directory', rows.fetch('/directory')[:type]
    assert_equal 'fifo', rows.fetch('/fifo')[:type]
    assert_equal 'characterSpecial', rows.fetch('/null')[:type]
    assert_equal 'blockSpecial', rows.fetch('/block')[:type]
    assert_equal 'socket', rows.fetch('/socket')[:type]
    assert_equal 4, rows.fetch('/link')[:size]
    assert_equal 'file', rows.fetch('/link')[:target]
    assert_equal 2052, rows.fetch('/BIG.BIN;1')[:size]
  end

  def test_source_manifest_uses_root_path_and_logical_symlink_size
    Dir.mktmpdir('iso-corpus-') do |directory|
      tree = File.join(directory, 'tree')
      Dir.mkdir(tree)
      File.symlink('target', File.join(tree, 'link'))
      manifest = File.join(directory, 'source.jsonl')
      IsoCorpus.source_manifest(tree, manifest)
      rows = File.readlines(manifest).map { JSON.parse(_1) }
      assert_equal '/', rows.first.fetch('path')
      assert_equal 6, rows.last.fetch('size')
    end
  end
end
