# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

require "fileutils"
require "open3"
require "tmpdir"

# Executable firmware evidence complements the structural catalog tests.
# Every VM and file belongs to one disposable fixture; no host disks attach.
module IsoBoot
  class Error < StandardError; end

  module_function

  def command(*arguments, expected: 0)
    output, errors, status = Open3.capture3(*arguments)
    raise Error, "#{arguments.first} failed: #{errors}" unless status.exitstatus == expected

    output + errors
  end

  def stop(child)
    Process.kill("KILL", -child.pid) if child.alive?
  rescue Errno::ESRCH
    # The VM exited between the liveness check and kill.
  end

  # Drain both pipes while waiting. A timeout kills the process group, and
  # cleanup also kills it if an exception interrupts the wait.
  def firmware(image:, code:, variables:, marker:, negative: false)
    drives = if variables
      ["-drive", "if=pflash,format=raw,readonly=on,file=#{code}",
       "-drive", "if=pflash,format=raw,file=#{variables}"]
    else
      ["-bios", code]
    end
    arguments = [ENV.fetch("QEMU_SYSTEM_X86_64", "qemu-system-x86_64"),
                 "-no-user-config",
                 "-machine", "pc,accel=tcg", "-m", "128M", "-display", "none", "-nic", "none",
                 "-monitor", "none", "-serial", "stdio", "-no-reboot",
                 "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
                 "-boot", "order=d,strict=on", "-cdrom", image, *drives]
    Open3.popen3(*arguments, pgroup: true) do |input, output, errors, child|
      input.close
      stdout = Thread.new { output.read }
      stderr = Thread.new { errors.read }
      begin
        timed_out = child.join(20).nil?
        stop(child) if timed_out
        status = child.value
        text = stdout.value
        diagnostics = stderr.value
        if negative
          raise Error, "corrupt catalog booted" if text.include?(marker)
          raise Error, "negative VM failed to start: #{diagnostics}" unless timed_out
        elsif timed_out || status.exitstatus != 33 || !text.include?(marker)
          raise Error, "firmware boot failed (timeout=#{timed_out}, #{status.inspect}): #{text} #{diagnostics}"
        end
      ensure
        stop(child)
        stdout.join
        stderr.join
      end
    end
  end

  def call(build:)
    helper = File.join(build, "test-iso9660-write")
    binary = File.join(build, "fsimg")
    fixtures = File.join(__dir__, "fixtures", "iso9660")
    Dir.mktmpdir("iso-boot-", build) do |scratch|
      tree = File.join(scratch, "tree")
      esp = File.join(scratch, "esp")
      FileUtils.mkdir_p([tree, File.join(esp, "EFI", "BOOT")])
      bios = File.join(tree, "bios.img")
      object = File.join(scratch, "efi.obj")
      application = File.join(esp, "EFI", "BOOT", "BOOTX64.EFI")
      command(ENV.fetch("NASM", "nasm"), "-f", "bin", "-o", bios,
              File.join(fixtures, "bios.asm"))
      command(ENV.fetch("EFI_CC", "clang"), "--target=x86_64-pc-windows-msvc",
              "-std=c11", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
              "-fno-stack-protector", "-mno-red-zone", "-O2", "-c",
              File.join(fixtures, "efi.c"), "-o", object)
      command(ENV.fetch("LLD_LINK", "lld-link"), "/subsystem:efi_application",
              "/entry:efi_main", "/nodefaultlib", "/machine:x64", "/timestamp:0",
              "/out:#{application}", object)
      command(binary, "c", "-S", "off", "-R", "-e", "1700000000",
              "-F", "fat12", "-s", "1440K", "-f", File.join(tree, "efi.img"),
              "-C", esp, ".")

      %w[bios efi both].each do |kind|
        image = File.join(scratch, "#{kind}.iso")
        command(helper, "--create-boot", image, tree,
                kind == "efi" ? "-" : "/bios.img", kind == "bios" ? "-" : "/efi.img")
        report = command("xorriso", "-no_rc", "-indev", image, "-report_el_torito", "plain")
        expected = kind == "both" ? 2 : 1
        raise Error, "xorriso boot-entry count differs" unless report.scan(/Boot img\s*:/i).size == expected

        unless kind == "efi"
          firmware(image: image, code: ENV.fetch("FSIMG_SEABIOS"), variables: nil,
                   marker: "FSIMG_BIOS_OK")
        end
        unless kind == "bios"
          variables = File.join(scratch, "#{kind}-vars.fd")
          FileUtils.cp(ENV.fetch("FSIMG_OVMF_VARS"), variables)
          firmware(image: image, code: ENV.fetch("FSIMG_OVMF_CODE"), variables: variables,
                   marker: "FSIMG_EFI_OK")
        end
        puts "El Torito #{kind}: xorriso and executable firmware passed"
      end

      image = File.join(scratch, "bios.iso")
      File.open(image, "r+b") do |file|
        file.seek(17 * 2048 + 71)
        catalog = file.read(4).unpack1("V")
        file.seek(catalog * 2048 + 28)
        byte = file.read(1).unpack1("C")
        file.seek(catalog * 2048 + 28)
        file.write([byte ^ 1].pack("C"))
      end
      report = command(binary, "W", "-S", "off", "-f", image, expected: 1)
      raise Error, "fsimg did not diagnose the bad checksum" unless report.include?("checksum")

      # SeaBIOS accepts a bad checksum. Its firmware refusal witness is a
      # bad validation key; checksum integrity is enforced by fsimg above.
      File.open(image, "r+b") do |file|
        file.seek(17 * 2048 + 71)
        catalog = file.read(4).unpack1("V")
        file.seek(catalog * 2048)
        entry = file.read(32)
        entry.setbyte(30, 0)
        entry[28, 2] = "\0\0".b
        entry[28, 2] = [(-entry.unpack("v16").sum) & 0xffff].pack("v")
        file.seek(catalog * 2048)
        file.write(entry)
      end
      firmware(image: image, code: ENV.fetch("FSIMG_SEABIOS"), variables: nil,
               marker: "FSIMG_BIOS_OK", negative: true)
      puts "El Torito corruption: fsimg rejected the checksum; BIOS refused the validation key"
    end
  end
end

if $PROGRAM_NAME == __FILE__
  begin
    IsoBoot.call(build: File.expand_path(ARGV.fetch(0)))
  rescue IsoBoot::Error, KeyError, Errno::ENOENT => error
    warn error.message
    exit 1
  end
end
