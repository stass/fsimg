# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_help_describes_accepted_options_and_escapes_errors

    empty = invoke(arguments: [], expected: 2)
    assert_equal("".b, empty.stdout, "empty stdout")
    assert(empty.stderr.start_with?("usage: fsimg".b), "empty stderr")
    assert_includes(empty.stderr, "modes:".b, "empty stderr")
    refute_includes(empty.stderr, "errno".b, "empty stderr")
    help_output = invoke(arguments: ["-h"])
    assert_equal(empty.stderr, help_output.stdout, "help output stdout")
    assert_equal("".b, help_output.stderr, "help output stderr")
    [
      ["c", "c[W]"],
      ["x", "x"],
      ["t", "t"],
      ["i", "i"],
      ["W", "W"],
      ["--formats", "--formats"]
    ].each do |mode, usage|
      help_output = invoke(arguments: [mode, "-h"])
      assert(help_output.stdout.start_with?("usage: fsimg #{usage} ".encode(Encoding::UTF_8).b), "help output stdout")
      assert_includes(help_output.stdout, "options:".b, "help output stdout")
      assert_includes(help_output.stdout, "example".b, "help output stdout")
      refute_includes(help_output.stdout, "modes:".b, "help output stdout")
      assert_equal("".b, help_output.stderr, "help output stderr")
    end
    # Each mode's help describes every option letter the mode accepts, as
    # the parser itself reports acceptance, except mode letters, -V and -M,
    # which is recognized only to be reported as unsupported.
    ["c", "cW", "x", "t", "W", "i"].each do |mode|
      mode_help = invoke(arguments: [mode, "-h"]).stdout.dup.force_encoding(Encoding::UTF_8)
      assert_includes(mode_help, "--compress=NAME")
      assert_includes(mode_help, "Same as --compress=gzip")
      accepted_letters(mode: mode).each do |letter|
        assert_includes(mode_help, "\n    -#{letter} ", [mode, letter])
      end
    end

    create_help = invoke(arguments: ["c", "-h"]).stdout.dup.force_encoding(Encoding::UTF_8)
    ["--free-space=", "--compress=NAME", "--json"].each do |option|
      assert_includes(create_help, "\n    #{option}", option)
    end

    assert_includes(create_help, "(.gz, .zst")
    assert_includes(create_help, "or .xz)")
    assert(Regexp.new("-F stack .*gpt", Regexp::MULTILINE).match(create_help), create_help)
    missing = invoke(arguments: ["i"], expected: 2)
    assert_equal("fsimg: -f image is required\nusage: fsimg i -f image [options]\n".b, missing.stderr)
    [
      [["i", "-Q"], "unknown option \"-Q\"".b],
      [["i", "-f"], "option requires an argument \"-f\"".b],
      [["no-such-command"], "unknown mode or option letter in the first argument \"-\"".b],
      [["extract"], "unknown mode or option letter in the first argument \"r\"".b],
      [[""], "the first argument names no mode".b],
      [["-v"], "a mode is required".b],
      [["-V", "x"], "-V takes no other option or operand".b]
    ].each do |arguments, detail|
      result = invoke(arguments: arguments, expected: 2)
      assert_includes(result.stderr, detail, [arguments, result.stderr])
      assert_includes(result.stderr, "usage:".b, [arguments, result.stderr])
      refute_includes(result.stderr, "errno".b, "result stderr")
      assert_equal("".b, result.stdout, "result stdout")
    end

    formats = invoke(arguments: ["--formats"])
    # The DMG and gzip rows are each followed by the zlib version fsimg runs
    # against.
    zlib_version = Regexp.new("\\n {22}(zlib [0-9][0-9A-Za-z.-]*)\\n".b).match(formats.stdout)
    refute_nil(zlib_version, formats.stdout)
    assert_equal(2, formats.stdout.scan(zlib_version[0]).size, formats.stdout)
    xz_version = if xz_supported?
      version = xz_format.fetch("codec_version")
      assert_match(/\Aliblzma [0-9][0-9A-Za-z.-]*\z/, version)
      "                      #{version}\n"
    else
      assert_nil(xz_format.fetch("codec_version"))
      ""
    end
    xz_capabilities = xz_supported? ? "yes   yes    yes" : "no    no     no "
    expected_formats = <<~FORMATS
      Format  Read  Write  Verify
      fat12   yes   yes    yes
      fat16   yes   yes    yes
      fat32   yes   yes    yes
      exfat   yes   yes    yes
      qnx6    yes   yes    yes
      qnx4    yes   yes    yes
      ifs     yes   yes    yes

      Layer    Kind         Read  Write  Verify  Codecs           Settings
      gpt      scheme       yes   yes    yes     -                sector,disk,type,name,disk-guid,guid
      mbr      scheme       yes   yes    yes     -                disk,type,active,id
      dmg      container    yes   yes    yes     raw,zero,zlib    compress,level,chunk,uuid
      vhd      container    yes   yes    yes     raw              align,uuid
      simg     container    yes   yes    yes     raw,zero,fill    block,crc
      ifsboot  container    yes   yes    yes     raw,lzo1x,nrv2b  startup,prefix,machine,virtual,startup_vaddr,paddr_bias,image_paddr,ram_paddr,imagefs_paddr,compress
      hbcifs   container    yes   no     yes     lzo1x            -
      gzip     compression  yes   yes    yes     -                level
      zstd     compression  #{zstd_supported? ? "yes   yes    yes" : "no    no     no "}     -                level
      xz       compression  #{xz_capabilities}     -                level,check
    FORMATS
    zstd_version = zstd_supported? ? "                      #{zstd_format.fetch("codec_version")}\n" : ""
    expected_formats.sub!("xz       compression", "#{zstd_version}xz       compression")
    expected_formats += xz_version
    assert_equal(expected_formats.b, formats.stdout.gsub(zlib_version[0], "\n".b))
    assert_equal("".b, formats.stderr)
    verbose = invoke(arguments: ["--formats", "-v"])
    assert_equal(formats.stdout, verbose.stdout, "verbose stdout")
    assert_includes(verbose.stderr, "Sandbox backend:".b, "verbose stderr")
    hostile_option = invoke(arguments: ["t", ("--" + ("\u001b\n" * 12000))], expected: 2)
    assert_includes(hostile_option.stderr, "unknown option".b, "hostile option stderr")
    assert_includes(hostile_option.stderr, "truncated".b, "hostile option stderr")
    assert_includes(hostile_option.stderr, "usage:".b, "hostile option stderr")
    refute_includes(hostile_option.stderr, "\x1b".b, "hostile option stderr")
    assert(hostile_option.stderr.size < 4096, "hostile option stderr size")
  end
end
