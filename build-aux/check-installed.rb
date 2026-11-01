# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

# Validate installed metadata without selecting another installed package.
require "fileutils"
require "open3"
require "pathname"
require "shellwords"
require "tmpdir"

module InstalledPackage
  class Error < StandardError
  end

  class QueryError < Error
  end

  module_function

  def path_flag?(flag:, marker:, expected:)
    return false unless flag.start_with?(marker)
    path = Pathname.new(flag.delete_prefix(marker))
    # Flags must work from every consumer's working directory. realpath
    # compares existing directories after resolving equivalent spellings.
    path.absolute? && path.realpath == expected.realpath
  rescue Errno::ENOENT, Errno::ENOTDIR, Errno::ELOOP
    false
  end

  def query(pkg_config:, prefix:, option:)
    environment = {
      "PATH" => ENV.fetch("PATH", "/usr/bin:/bin"),
      "LC_ALL" => "C",
      "PKG_CONFIG_PATH" => "",
      "PKG_CONFIG_LIBDIR" => (prefix / "lib/pkgconfig").to_s
    }
    output, status = Open3.capture2(
      environment,
      pkg_config,
      option,
      "libfsimg",
      unsetenv_others: true
    )
    raise QueryError, "pkg-config query failed" unless status.success?
    output.strip
  end

  def validate(pkg_config:, prefix:, version:)
    answer = -> (option) { query(pkg_config: pkg_config, prefix: prefix, option: option) }
    unless answer.call("--modversion") == version
      raise InstalledPackage::Error, "installed package version differs from the command"
    end

    cflags = Shellwords.split(answer.call("--cflags"))
    libraries = Shellwords.split(answer.call("--libs"))
    unless cflags.size == 1 &&
        path_flag?(
          flag: cflags.first,
          marker: "-I",
          expected: prefix / "include"
        )
      raise InstalledPackage::Error, "unexpected installed Cflags: #{cflags.inspect}"
    end

    unless libraries.size == 2 &&
        path_flag?(flag: libraries.first, marker: "-L", expected: prefix / "lib") &&
        libraries.last == "-lfsimg"
      raise InstalledPackage::Error, "unexpected installed Libs: #{libraries.inspect}"
    end
  end

  def self_test(pkg_config)
    Dir.mktmpdir("fsimg-pc-") do |temporary|
      prefix = Pathname.new(temporary) / "prefix with spaces"
      directory = prefix / "lib/pkgconfig"
      FileUtils.mkdir_p(directory)
      FileUtils.mkdir_p(prefix / "include")
      metadata = directory / "libfsimg.pc"
      good = File.read(File.expand_path("../libfsimg.pc.in", __dir__))
      replacements = {
        "@prefix@" => "#{prefix.parent}//#{prefix.basename}",
        "@exec_prefix@" => "${prefix}",
        "@libdir@" => "${exec_prefix}/lib",
        "@includedir@" => "${prefix}/include",
        "@PACKAGE_VERSION@" => "1.2.3",
        "@ZLIB_LIBS@" => "-lz",
        "@ZSTD_LIBS@" => "",
        "@LZMA_LIBS@" => ""
      }
      good = replacements.reduce(good) { |text, (key, value)| text.gsub(key, value) }
      metadata.write(good)
      validate(pkg_config: pkg_config, prefix: prefix, version: "1.2.3")
      changes = [
        ["Cflags: -I\"${includedir}\"", "Cflags: -I/another/include"],
        ["Libs: -L\"${libdir}\" -lfsimg", "Libs: -L/another/lib -lfsimg"],
        ["Cflags: -I\"${includedir}\"", "Cflags: -I\"#{(prefix / "include").relative_path_from(Pathname.pwd)}\""],
        ["Libs: -L\"${libdir}\" -lfsimg", "Libs: -L\"#{(prefix / "lib").relative_path_from(Pathname.pwd)}\" -lfsimg"],
        ["-lfsimg\n", "-lfsimg -lcrypto\n"]
      ]
      changes.each do |old, replacement|
        raise InstalledPackage::Error, "self-test replacement is absent" unless good.include?(old)
        metadata.write(good.gsub(old, replacement))
        begin
          validate(pkg_config: pkg_config, prefix: prefix, version: "1.2.3")
        rescue QueryError
          raise
        rescue InstalledPackage::Error
          next
        end

        raise InstalledPackage::Error, "malformed installed metadata was accepted"
      end
    end

    puts("installed-metadata rejection checks pass")
  end

  def call(pkg_config:, prefix:)
    required = %w[
      include/fsimg.h
      lib/libfsimg.a
      share/man/man1/fsimg.1
      share/man/man3/libfsimg.3
      share/fsimg/examples/provider.c
    ]
    required.each do |relative|
      raise InstalledPackage::Error, "installed artifact is missing: #{relative}" unless (prefix / relative).file?
    end

    shared = RUBY_PLATFORM.include?("darwin") ? "lib/libfsimg.0.dylib" : "lib/libfsimg.so.0"
    raise InstalledPackage::Error, "shared library is missing from the qualification build" unless (prefix / shared).file?
    version, status = Open3.capture2((prefix / "bin/fsimg").to_s, "-V")
    unless status.success? && version.strip.start_with?("fsimg ")
      raise InstalledPackage::Error, "installed command returned an invalid version"
    end

    validate(pkg_config: pkg_config, prefix: prefix, version: version.strip.delete_prefix("fsimg "))
    puts("installed package paths, version, Cflags and Libs pass")
  end
end

case ARGV
in ["--self-test", pkg_config]
  InstalledPackage.self_test(pkg_config)
in [pkg_config, prefix]
  InstalledPackage.call(pkg_config: pkg_config, prefix: Pathname.new(prefix))
else
  abort("usage: check-installed.rb [--self-test PKG_CONFIG | PKG_CONFIG PREFIX]")
end
