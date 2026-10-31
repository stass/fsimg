# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_ifsboot_creation_validates_settings_and_startup_checksums
    root = @root
    # A boot image through the command: the startup file reaches the worker
    # as a descriptor of its own and follows the header with its padding, the
    # sums are zero, and the image lists, verifies and is reproducible; machine
    # 3 behind the BIOS loader's prefix is written raw. A file setting that is
    # a symbolic link, a directory or missing, a machine and mode the table
    # does not list as writable, and a prefix without its machine are refused
    # with no output left; a read mode refuses the key.
    startup = (0...4001).map { |_| Random.new(5).rand(256) }.to_a.pack("C*")
    (root / "startup.bin").binwrite(startup)
    (root / "prefix.bin").binwrite("\xfa".b + ("\0".b * 1084))
    File.symlink("startup.bin", (root / "startup-link.bin"))
    settings = [
      "-o",
      "ifsboot:machine=0",
      "-o",
      "ifsboot:virtual=no",
      "-o",
      "ifsboot:startup_vaddr=0x1000",
      "-o",
      "ifsboot:paddr_bias=0x0",
      "-o",
      "ifsboot:image_paddr=0x2000",
      "-o",
      "ifsboot:ram_paddr=0x3000",
      "-o",
      "ifsboot:imagefs_paddr=0x0"
    ]
    plain = ["c", "-R", "-S", "off", "-F", "ifs/ifsboot", *settings]
    base = [plain[0], "--json", *plain[1..]]
    operands = ["-C", "source", "."]
    ["lzo", "none"].each do |mode|
      arguments = [*base, "-o", "ifsboot:compress=#{mode}", "-o", "ifsboot:startup=startup.bin"]
      name = "boot-#{mode}.ifs"
      completed(invoke(arguments: [*arguments, "-f", name, *operands]))
      image = (root / name).binread
      header = boot_header(image)
      assert_equal(0x00ff7eeb, header.fetch("signature"), header)
      assert_equal(0, header.fetch("machine"), header)
      assert_equal(((mode == "lzo") ? 0x08 : 0x00), header.fetch("flags1"), header)
      assert_equal((256 + 4004) + 4, header.fetch("startup_size"), "header startup size")
      assert_equal(image.size, header.fetch("stored_size"), "header stored size")
      assert_equal(0, header.fetch("ram_size"), header)
      assert_equal(startup, image[256...(256 + 4001)], "image 256 256 4001")
      assert_equal("\0".b * 3, image[(256 + 4001)...(256 + 4004)], "image 256 4001 256 4004")
      assert_equal(0, word_sum(image[0...header.fetch("startup_size")]), "word sum image 0 header startup size")
      assert_equal(0, word_sum(image), "word sum image")
      assert_equal(
        ["\"/HELLO.TXT\"", "\"/nested\"", "\"/nested/caf\u00e9.txt\""],
        listing(image: name).sort,
        name
      )
      verified = invoke(arguments: ["W", "-d", "-f", name]).stdout
      assert_includes(verified, "ifsboot container,".b, verified)
      completed(invoke(arguments: [*arguments, "-f", "again.ifs", *operands]))
      same_bytes(first: (root / name), second: (root / "again.ifs"))
      (root / "again.ifs").unlink
      (root / name).unlink
    end

    plan = records(invoke(arguments: [*base, "-n", "-o", "ifsboot:startup=startup.bin", *operands]))
    assert(Integer(plan.find { |record| record.fetch("type") == "plan" }.fetch("output_bytes"), 10) > 0, "Integer plan find record record type")
    [
      [["-o", "ifsboot:startup=startup-link.bin"], "file setting is a symbolic link".b],
      [["-o", "ifsboot:startup=source"], "file setting is not a regular file".b],
      [["-o", "ifsboot:prefix=missing.bin"], "cannot open file setting".b],
      [["-o", "ifsboot:prefix=prefix.bin"], "machine 0 is not supported behind this boot prefix".b]
    ].each do |extra, detail|
      result = invoke(arguments: [*plain, *extra, "-f", "refused.ifs", *operands], expected: 1)
      assert_includes(result.stderr, detail, [extra, result.stderr])
      refute((root / "refused.ifs").exist?, "refused creation leaves no output image")
    end
    # A file setting of a kind no setting reads is unsupported, as such an
    # image or operand is, not an I/O failure, and a trailing "/" or "/."
    # never makes the open follow a link.
    File.mkfifo(root / "setting-fifo")
    listener = Socket.new(:UNIX, :STREAM)
    listener.bind(Socket.sockaddr_un(relative_path(root / "setting-socket")))
    File.symlink("source", (root / "setting-directory-link"))
    symlinks = [
      "startup-link.bin",
      "startup-link.bin/",
      "startup-link.bin/.",
      "setting-directory-link/."
    ]
    [*symlinks, "source", "setting-fifo", "setting-socket"].each do |setting|
      error = failed(
        invoke(
          arguments: [
            *base,
            "-o",
            "ifsboot:startup=#{setting}",
            "-f",
            "refused.ifs",
            *operands
          ],
          expected: 1
        )
      )
      assert_equal("unsupported capability", error.fetch("status"), error)
      assert_equal(setting, error.fetch("path"), error)
      if symlinks.include?(setting)
        assert_equal(Errno::ELOOP::Errno, error.fetch("os_error"), error)
      end

      refute((root / "refused.ifs").exist?, "refused creation leaves no output image")
    end

    (root / "setting-fifo").unlink
    listener.close
    (root / "setting-socket").unlink
    (root / "setting-directory-link").unlink
    unwritable = plain.map { |arg| arg.gsub("machine=0", "machine=42") }
    result = invoke(arguments: [*unwritable, "-f", "refused.ifs", *operands], expected: 1)
    assert_includes(result.stderr, "machine 42 is not writable under compress=lzo".b, result.stderr)
    refute((root / "refused.ifs").exist?, "refused creation leaves no output image")
    # Machine 3 behind the BIOS loader is written raw, the one mode an x86
    # boot has passed, and refused under the default lzo.
    x86_files = ["-o", "ifsboot:prefix=prefix.bin", "-o", "ifsboot:startup=startup.bin"]
    json_x86 = [*base.map { |arg| arg.gsub("machine=0", "machine=3") }, *x86_files]
    x86 = [*plain.map { |arg| arg.gsub("machine=0", "machine=3") }, *x86_files]
    completed(
      invoke(
        arguments: [
          *json_x86,
          "-o",
          "ifsboot:compress=none",
          "-f",
          "x86.ifs",
          *operands
        ]
      )
    )
    image = (root / "x86.ifs").binread
    header = boot_header(image[1088..])
    assert_equal(("\xfa".b + ("\0".b * 1087)), image[0...1088], image[0...16])
    assert_equal(3, header.fetch("machine"), "header machine")
    assert_equal(0x00, header.fetch("flags1"), "header flags1")
    assert_equal(1088, header.fetch("preboot_size"), "header preboot size")
    assert_equal(header.fetch("stored_size"), header.fetch("ram_size"), header)
    assert_equal(image.size - 1088, header.fetch("stored_size"), header)
    assert_equal(0, word_sum(image[1088...(1088 + header.fetch("startup_size"))]))
    assert_equal(0, word_sum(image[1088..]))
    assert_includes(invoke(
        arguments: [
          "W",
          "-d",
          "-f",
          "x86.ifs"
        ]
      )
        .stdout, "ifsboot container,".b, "invoke arguments W d f x86 ifs stdout")
    (root / "x86.ifs").unlink
    result = invoke(arguments: [*x86, "-f", "refused.ifs", *operands], expected: 1)
    assert_includes(result.stderr, "machine 3 is not writable under compress=lzo".b, result.stderr)
    refute((root / "refused.ifs").exist?, "refused creation leaves no output image")
    result = invoke(
      arguments: [
        "t",
        "-f",
        "startup.bin",
        "-F",
        "ifs/ifsboot",
        "-o",
        "ifsboot:startup=startup.bin"
      ],
      expected: 2
    )
    assert_includes(result.stderr, "setting applies only to creation".b, result.stderr)
    ["startup.bin", "prefix.bin", "startup-link.bin"].each do |name|
      (root / name).unlink
    end
  end
end
