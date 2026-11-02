# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require 'digest'
require 'fileutils'
require 'find'
require 'json'
require 'open3'
require 'shellwords'

module IsoCorpus
  class Error < StandardError; end
  class ProducerError < Error; end
  class ConsumerError < Error; end
  Invocation = Data.define(:name, :tree, :options)
  CommandResult = Data.define(:stdout, :stderr, :status)

  module_function

  def executable(name)
    ENV.fetch('PATH').split(File::PATH_SEPARATOR).map { File.join(_1, name) }
      .find { File.file?(_1) && File.executable?(_1) }&.then { File.realpath(_1) }
  end

  # A watchdog kills the process group, including descendants holding pipes.
  # The bound allows sparse multi-extent producers and consumers to finish.
  def subprocess(command, environment: {}, seconds: 600, error_class: ConsumerError)
    Open3.popen3(environment, *command, pgroup: true) do |input, output, errors, child|
      timed_out = false
      watchdog = Thread.new do
        sleep seconds
        timed_out = true
        Process.kill('KILL', -child.pid)
      rescue Errno::ESRCH
        # The child exited just before the deadline.
      end
      input.close
      begin
        result = yield output, errors, child
        raise error_class, "#{Shellwords.join(command)} timed out after #{seconds} seconds" if timed_out

        result
      ensure
        begin
          Process.kill('KILL', -child.pid) if child.alive?
        rescue Errno::ESRCH
          # The child exited before cleanup.
        end
        watchdog.kill
        watchdog.join
      end
    end
  end

  def capture_result(*command, environment: {}, error_class: ConsumerError)
    subprocess(command, environment: environment, error_class: error_class) do |output, errors, child|
      diagnostics = Thread.new { errors.read }
      text = output.read
      CommandResult.new(stdout: text, stderr: diagnostics.value, status: child.value)
    end
  end

  def capture(*command, environment: {}, combined: false, diagnostics: nil)
    result = capture_result(*command, environment: environment)
    File.write(diagnostics, result.stderr) if diagnostics
    unless result.status.success?
      raise ConsumerError, "#{Shellwords.join(command)} failed (#{result.status.inspect}): #{result.stdout}#{result.stderr}"
    end

    combined ? result.stdout + result.stderr : result.stdout
  end

  def dialect(version)
    return 'xorriso' if version.include?('xorriso')
    return 'genisoimage' if version.include?('genisoimage')
    return 'mkisofs' if version.include?('mkisofs')

    raise ProducerError, "unknown ISO producer version: #{version.strip}"
  end

  def write(tree:, path:, content:)
    File.binwrite(File.join(tree, path), content)
  end

  # Rows are producer invocations, not mutations of another producer's image.
  def invocations(tool, sources)
    prefix = tool == 'xorriso' ? ['-no_rc', '-as', 'mkisofs'] : []
    efi = tool == 'mkisofs' ? ['-eltorito-platform', 'efi', '-b'] : ['-e']
    row = ->(name:, tree:, options: []) { Invocation.new(name: name, tree: tree, options: options) }
    rows = [
      row.call(name: 'empty', tree: 'empty'),
      row.call(name: 'minimal', tree: 'minimal'),
      *[1, 2, 3].map { row.call(name: "level#{_1}", tree: 'names', options: ['-iso-level', _1.to_s]) },
      row.call(name: 'rr', tree: 'rr', options: ['-R']),
      row.call(name: 'deep', tree: 'deep', options:
               tool == 'xorriso' ? ['-R', '-rr_reloc_dir', 'rr_moved'] : ['-R']),
      row.call(name: 'joliet', tree: 'joliet', options: ['-R', '-J']),
      row.call(name: 'joliet-long', tree: 'joliet', options: ['-R', '-J', '-joliet-long']),
      row.call(name: 'multi', tree: 'large', options: ['-R', '-iso-level', '3']),
      row.call(name: 'bios', tree: 'boot', options: ['-R', '-b', 'bios.img', '-no-emul-boot']),
      row.call(name: 'efi', tree: 'boot', options: ['-R', *efi, 'efi.img', '-no-emul-boot']),
      row.call(name: 'both', tree: 'boot', options: ['-R', '-b', 'bios.img', '-no-emul-boot',
               '-eltorito-alt-boot', *efi, 'efi.img', '-no-emul-boot']),
      row.call(name: 'hidden', tree: 'boot', options: ['-R', '-b', 'bios.img', '-no-emul-boot',
               '-hide', 'boot.cat', '-hide-joliet', 'boot.cat'])
    ]
    if tool == 'xorriso'
      rows += [
        row.call(name: 'joliet-utf16', tree: 'joliet', options: ['-R', '-J', '-joliet-utf16']),
        row.call(name: 'isohybrid', tree: 'boot', options: ['-R', '-b', 'bios.img', '-no-emul-boot',
                 '-eltorito-alt-boot', *efi, 'efi.img', '-no-emul-boot',
                 '-isohybrid-gpt-basdat', '-part_like_isohybrid'])
      ]
    elsif tool == 'mkisofs'
      rows += [row.call(name: 'rr112', tree: 'rr', options: ['-R', '-rrip112'])]
    end
    rows.map do |item|
      catalog = item.tree == 'boot' ? ['-c', 'boot.cat'] : []
      Invocation.new(name: "#{tool}-#{item.name}", tree: File.join(sources, item.tree),
                     options: prefix + ['-V', 'FSIMG', '-quiet', '-input-charset', 'utf-8'] +
                       catalog + item.options + ['-o', '-', File.join(sources, item.tree)])
    end
  end

  # Seek over zero chunks so a multi-extent fixture costs metadata, not 4 GiB
  # of allocated disk. Hash the actual stream, including its holes.
  def produce(command:, image:, log:, environment: {})
    digest = Digest::SHA256.new
    status = nil
    subprocess(command, environment: environment, error_class: ProducerError) do |output, errors, child|
      diagnostics = Thread.new { File.open(log, 'wb') { IO.copy_stream(errors, _1) } }
      File.open(image, 'wb') do |file|
        while (chunk = output.read(1024 * 1024))
          digest.update(chunk)
          if chunk.count("\0") == chunk.bytesize
            file.seek(chunk.bytesize, IO::SEEK_CUR)
          else
            file.write(chunk)
          end
        end
        file.truncate(file.pos)
      end
      diagnostics.value
      status = child.value
    end
    unless status.success?
      raise ProducerError, "exit=#{status.exitstatus.inspect}, signal=#{status.termsig.inspect}; see #{log}"
    end

    digest.hexdigest
  end

  def content_digest(command, environment: {})
    digest = Digest::SHA256.new
    size = 0
    subprocess(command, environment: environment) do |output, errors, child|
      diagnostics = Thread.new { errors.read }
      while (chunk = output.read(1024 * 1024))
        digest.update(chunk)
        size += chunk.bytesize
      end
      message = diagnostics.value
      status = child.value
      unless status.success?
        raise ConsumerError, "#{Shellwords.join(command)} content read failed " \
                             "(exit=#{status.exitstatus.inspect}, signal=#{status.termsig.inspect}): #{message}"
      end
    end
    [digest.hexdigest, size]
  end

  def source_rows(tree)
    Find.find(tree).map do |entry|
      stat = File.lstat(entry)
      path = entry.delete_prefix(tree)
      target = stat.symlink? ? File.readlink(entry) : nil
      { path: path.empty? ? '/' : path, type: stat.ftype,
        size: target ? target.bytesize : (stat.file? ? stat.size : 0), target: target,
        sha256: stat.file? ? Digest::SHA256.file(entry).hexdigest : nil }
    end
  end

  def write_manifest(path, rows)
    File.open(path, 'w') { |file| rows.each { file.puts JSON.generate(_1) } }
  end

  def source_manifest(tree, path) = write_manifest(path, source_rows(tree))

  def list_view(image:, flags:, base:)
    listing = capture('isoinfo', *flags, '-l', '-i', image, diagnostics: base + '.listing.stderr')
    File.write(base + '.listing', listing)
    paths = capture('isoinfo', *flags, '-f', '-i', image, diagnostics: base + '.paths.stderr')
    File.write(base + '.paths', paths)
    [decode_listing(listing), paths.lines.map(&:strip).tally]
  end

  def cross_check_listing(rows:, occurrences:, base:)
    missing = occurrences.keys - rows.keys
    raise ConsumerError, "listing parser misses isoinfo -f entries: #{base}" unless missing.empty?

    extra = rows.keys - occurrences.keys
    File.write(base + '.status', "oracle=isoinfo; Joliet non-BMP names can be lossy\n" \
                                "relocation entries listed by -l but omitted by -f: #{extra.size}\n")
  end

  def hash_view(image:, flags:, rows:, occurrences:, expected:)
    rows.each_value do |row|
      next unless row[:type] == 'file'

      digest, size = content_digest(['isoinfo', *flags, '-i', image, '-x', row[:path]])
      row[:sections] = occurrences.fetch(row[:path], 0)
      if expected
        unless expected[:size] == row[:size]
          raise ConsumerError, "unexpected oracle listing size: #{image}"
        end

        row[:sha256] = expected[:sha256]
        row[:sha256_source] = 'source tree (large multi-extent fixture)'
        row[:oracle_size] = size
        row[:oracle_sha256] = digest
      else
        raise ConsumerError, "unexpected oracle extraction size: #{image}" unless size == row[:size]

        row[:sha256] = digest
      end
    end
  end

  def decode_listing(listing)
    directory = '/'
    rows = {}
    listing.each_line do |line|
      if line.start_with?('Directory listing of ')
        directory = line.delete_prefix('Directory listing of ').strip
        next
      end
      match = line.match(/(?:^|\s)([bcdflps-][rwxstST-]{9})\s+.*\[\s*-?\d+\s+[0-9A-Fa-f]+\]\s+(.*)/)
      next unless match

      name, target = match[2].strip.split(' -> ', 2)
      next if %w[. ..].include?(name)

      path = directory.delete_suffix('/') + '/' + name
      fields = line.split
      mode_index = fields.index(match[1])
      size = fields.fetch(mode_index + 4).to_i
      type = { '-' => 'file', 'd' => 'directory', 'l' => 'link', 'p' => 'fifo', 'f' => 'fifo',
               'c' => 'characterSpecial', 'b' => 'blockSpecial', 's' => 'socket' }.fetch(match[1][0])
      if rows.key?(path)
        rows.fetch(path)[:size] += size
      else
        rows[path] = { path: path, type: type, size: size, target: target, sha256: nil }
        rows[path][:size] = target.bytesize if type == 'link' && target
      end
    end
    rows
  end

  # The CLI process gives every producer and consumer one private working
  # directory and temporary directory. Crashing external tools emit no cores.
  def prepare_output(path)
    repository = File.realpath(File.join(__dir__, '..'))
    requested = File.expand_path(path)
    parent = File.realpath(File.dirname(requested))
    output = File.join(parent, File.basename(requested))
    if output == repository || output.start_with?(repository + '/')
      abort 'output must be outside the repository'
    end
    Dir.mkdir(output, 0o700) # Exclusive creation also rejects symlink destinations.
    # Readable empty files stop the producers' executable-directory fallback.
    %w[.mkisofsrc .genisoimagerc].each { File.write(File.join(output, _1), '') }
    Dir.chdir(output)
    ENV['TMPDIR'] = File.join(output, 'tmp')
    Dir.mkdir(ENV.fetch('TMPDIR'), 0o700)
    Process.setrlimit(Process::RLIMIT_CORE, 0, 0)
    output
  end

  class Corpus
    include IsoCorpus

    def initialize(output)
      @output = output
      @source_manifests = {}
    end

    def producer_environment
      { 'HOME' => @output, 'MKISOFSRC' => nil, 'GENISOIMAGERC' => nil,
        'SOURCE_DATE_EPOCH' => '946684800' }
    end

    def view_manifest(image:, view:, flags:, source:)
      base = image.delete_suffix('.iso') + ".#{view}"
      rows, occurrences = list_view(image: image, flags: flags, base: base)
      cross_check_listing(rows: rows, occurrences: occurrences, base: base)
      # Large-file expectations come from fixture records, independently of
      # the consumer's ability to extract a complete multi-extent file.
      expected = source.find { _1[:type] == 'file' && _1[:size] > 0xffffffff }
      hash_view(image: image, flags: flags, rows: rows, occurrences: occurrences, expected: expected)
      rows.each_value do |row|
        next unless row.key?(:oracle_size)
        next if row[:oracle_size] == row[:size] && row[:oracle_sha256] == row[:sha256]

        message = "#{File.basename(image)} #{view}: isoinfo -x returns #{row[:oracle_size]} of #{row[:size]} bytes; expected digest from source"
        File.open(base + '.status', 'a') { _1.puts message }
        skip(message)
      end
      write_manifest(base + '.jsonl', rows.values.sort_by { _1[:path] })
    end

    def skip(message)
      puts "skip: #{message}"
      File.open(File.join(@output, 'skips.txt'), 'a') { _1.puts "skip: #{message}" }
    end

    def prepare_sources
      sources = File.join(@output, 'sources')
      %w[empty minimal names rr deep joliet large boot].each do |name|
        FileUtils.mkdir_p(File.join(sources, name))
      end
      write(tree: File.join(sources, 'minimal'), path: 'ONE.TXT', content: "one\n")
      names = File.join(sources, 'names')
      %w[A A0 A_ A.TXT A.B A.BB FOO. FOO.TXT lowercase].each do |name|
        write(tree: names, path: name, content: "#{name}\n")
      end
      %w[FOO AB A_B].each { FileUtils.mkdir_p(File.join(names, _1)) }
      write(tree: names, path: 'EMPTY', content: '')
      rr = File.join(sources, 'rr')
      write(tree: rr, path: 'file', content: "rock ridge\n")
      write(tree: rr, path: 'empty', content: '')
      FileUtils.mkdir_p(File.join(rr, 'directory'))
      File.symlink('file', File.join(rr, 'link'))
      File.mkfifo(File.join(rr, 'fifo'))
      # Device fixtures must never create host driver endpoints.
      skip('device files: privileged mknod fixtures omitted')
      24.times { write(tree: rr, path: format('%02d-', _1) + ('n' * 235), content: "long name\n") }
      names = (0...20).map { format('D%02d', _1) }
      (1..names.size).map { names.take(_1) }.each do |ancestors|
        deep = File.join(sources, 'deep', *ancestors)
        FileUtils.mkdir_p(deep)
        write(tree: deep, path: 'FILE', content: "depth #{ancestors.size - 1}\n")
      end
      joliet = File.join(sources, 'joliet')
      [62, 64, 103].each do |length|
        write(tree: joliet, path: format('f%03d-', length) + ('f' * (length - 5)), content: "length #{length}\n")
        FileUtils.mkdir_p(File.join(joliet, format('d%03d-', length) + ('d' * (length - 5))))
      end
      write(tree: joliet, path: "caf\u00e9", content: "accent\n")
      write(tree: joliet, path: "nonbmp-\u{1f600}", content: "non-BMP\n")
      File.open(File.join(sources, 'large', 'BIG.BIN'), 'wb') do |file|
        file.write("begin\n")
        file.seek((1 << 32) + 4096)
        file.write("end\n")
      end
      # Structural boot fixtures, not executable BIOS or EFI applications.
      write(tree: File.join(sources, 'boot'), path: 'bios.img', content: "\0" * 2048)
      write(tree: File.join(sources, 'boot'), path: 'efi.img', content: "\0" * (1440 * 1024))
      sources
    end

    def record_image(name:, tool:, version:, command:, tree:, direct: false)
      image = File.join(@output, "#{name}.iso")
      puts "produce: #{name}"
      if direct
        result = capture_result(*command, environment: producer_environment, error_class: ProducerError)
        diagnostics = result.stdout + result.stderr
        status = result.status
        File.write(image + '.log', diagnostics)
        unless status.success?
          raise ProducerError, "exit=#{status.exitstatus.inspect}, signal=#{status.termsig.inspect}; see #{image}.log"
        end
        digest = Digest::SHA256.file(image).hexdigest
      else
        digest = produce(command: command, image: image, log: image + '.log', environment: producer_environment)
      end
      row = { name: name, tool: tool, version: version, command: Shellwords.join(command),
              stdout: image, sha256: digest }
      File.open(File.join(@output, 'images.jsonl'), 'a') { _1.puts JSON.generate(row) }
      source = @source_manifests[tree] ||= source_rows(tree)
      write_manifest(image.delete_suffix('.iso') + '.source.jsonl', source)
      if executable('isoinfo')
        description = capture('isoinfo', '-d', '-i', image)
        File.write(image + '.descriptors', description)
        { 'primary' => [], 'rockridge' => ['-R'], 'joliet' => ['-J', '-j', 'utf-8'] }.each do |view, flags|
          next if view == 'rockridge' && !description.include?('Rock Ridge signatures')
          next if view == 'joliet' && !description.include?('Joliet with')

          view_manifest(image: image, view: view, flags: flags, source: source)
        end
      end
      return unless executable('bsdtar')

      # @archive reads via libarchive; mtree records names, kinds, sizes,
      # link targets and SHA256 without extracting special files on the host.
      manifest = image.delete_suffix('.iso') + '.bsdtar.mtree'
      result = capture_result('bsdtar', '-cf', manifest, '--format=mtree',
                              '--options', 'sha256', "@#{image}")
      diagnostics = result.stdout + result.stderr
      status = result.status
      File.write(manifest + '.status', "#{status.inspect}\n#{diagnostics}")
      raise ConsumerError, "bsdtar failed; see #{manifest}.status" unless status.success?
    rescue ConsumerError => error
      File.write(image + '.status', "consumer failed: #{error.message}\n")
      skip("#{name}: consumer failed: #{error.message}")
    rescue ProducerError => error
      # A producer failure is evidence too. Do not publish a partial ISO as a
      # successful manifest row, and let the remaining independent cases run.
      File.rename(image, image + '.failed') if File.exist?(image)
      skip("#{name}: producer failed: #{error.message}")
    end

    def run
      sources = prepare_sources
      %w[isoinfo bsdtar].each { skip("#{_1} missing") unless executable(_1) }
      seen = []
      %w[xorriso mkisofs genisoimage bsdtar makefs].each do |name|
        program = executable(name)
        if !program || seen.include?(program)
          skip("#{name}: #{program ? 'duplicate executable' : 'missing'}")
          next
        end
        seen << program
        begin
          case name
          when 'bsdtar'
            version = capture(program, '--version', environment: producer_environment, combined: true).strip
            %w[empty minimal names rr deep joliet].each do |tree|
              source = File.join(sources, tree)
              record_image(name: "bsdtar-#{tree}", tool: name, version: version,
                           command: [program, '-cf', '-', '--format=iso9660', '-C', source, '.'], tree: source)
            end
            skip('bsdtar multi-extent producer: libarchive stages payloads in temporary files')
          when 'makefs'
            # FreeBSD and kusumi/makefs share the cd9660 backend and -o syntax.
            result = capture_result(program, '-V', environment: producer_environment)
            version = result.stdout + result.stderr
            status = result.status
            version = "unreported (-V unsupported); executable SHA256 #{Digest::SHA256.file(program).hexdigest}" unless status.success?
            %w[empty minimal names rr deep].each do |tree|
              image_name = "makefs-#{tree}"
              source = File.join(sources, tree)
              record_image(name: image_name, tool: name, version: version.strip,
                           command: [program, '-t', 'cd9660', '-o', 'rockridge',
                                     File.join(@output, "#{image_name}.iso"), source], tree: source, direct: true)
            end
          else
            flags = name == 'xorriso' ? ['-no_rc', '-version'] : ['-version']
            version = capture(program, *flags, environment: producer_environment, combined: true).strip
            tool = dialect(version)
            invocations(tool, sources).each do |item|
              record_image(name: item.name, tool: tool, version: version,
                           command: [program] + item.options, tree: item.tree)
            end
          end
        rescue Error => error
          skip("#{name}: #{error.message}")
        end
      end
      skip('pycdlib optional producer not implemented; see docs/build.md for venv installation')
      skip('Joliet levels 1 and 2: native producers emit level 3; no level selector')
      skip('mkisofs/genisoimage isohybrid: no Syslinux MBR supplied; xorriso supplies partition-table coverage')
      puts "corpus: #{@output}"
    end

    private :skip, :prepare_sources
  end
end

if $PROGRAM_NAME == __FILE__
  abort 'usage: sh test/iso9660-corpus.sh OUTPUT-DIRECTORY (new, outside repository)' unless ARGV.size == 1
  IsoCorpus::Corpus.new(IsoCorpus.prepare_output(ARGV.first)).run
end
