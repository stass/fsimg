# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "minitest"
require "pathname"
require "rbconfig"
require "tmpdir"
require "fsimg_spec"

# The oracles' tools and shell (specs/design-spec-testing.md#runner): a
# tool is configured by absolute path only, and a shell refuses to run an
# oracle once its mtoolsrc is no longer empty.
class TestFixtures < Minitest::Test
  # Tools run from other directories, so a path relative to the harness's
  # working directory, even one that names an executable from here, is
  # no tool; nor is a directory.
  def test_a_tool_is_found_only_by_an_absolute_path_to_a_file
    relative = Pathname(RbConfig.ruby).relative_path_from(Dir.pwd).to_s
    found = FsimgSpec::Tools.find("MDIR" => RbConfig.ruby, "MCOPY" => relative, "FATCAT" => File.dirname(RbConfig.ruby))

    assert_equal RbConfig.ruby, found[:mdir]
    assert_nil found[:mcopy], relative
    assert_nil found[:fatcat]
  end

  # oracle-home/ holds the shell's .mtoolsrc alone, so any other name in
  # it, such as a configuration under another name, is refused.
  def test_a_shell_refuses_to_run_once_its_home_holds_another_name
    Dir.mktmpdir do |directory|
      home = File.join(directory, "home")
      shell = FsimgSpec::OracleShell.new(home:, environment: { "PATH" => "/bin:/usr/bin" })
      File.write(File.join(home, "mtoolsrc"), "drive a: file=\"/dev/null\"\n")

      error = assert_raises(FsimgSpec::FixtureError) { shell.run(["/usr/bin/true"], chdir: directory) }
      assert_match(/holds .*"mtoolsrc".*, not \.mtoolsrc alone\z/, error.message)
    end
  end

  # mtools reads HOME/.mtoolsrc as well as the file MTOOLSRC names, so a
  # configuration written to HOME/.mtoolsrc is refused too.
  def test_a_shell_refuses_to_run_once_home_mtoolsrc_is_written
    Dir.mktmpdir do |directory|
      home = File.join(directory, "home")
      shell = FsimgSpec::OracleShell.new(home:, environment: { "PATH" => "/bin:/usr/bin" })
      File.write(File.join(home, ".mtoolsrc"), "drive t: file=\"image.img\"\n")

      assert_raises(FsimgSpec::FixtureError) { shell.run(["/usr/bin/true"], chdir: directory) }
    end
  end

  # Only a disagreement is excused: it keeps its detail and names the
  # exception that excuses it, and an excused observation always names
  # one, so none is excused silently.
  def test_an_excused_observation_is_a_disagreement_naming_its_exception
    excused = FsimgSpec::Observation.disagrees("fsck_msdosfs -n exited 8").excused_by("t.exception")

    assert_equal({ state: "excused", detail: "fsck_msdosfs -n exited 8", exception_id: "t.exception" }, excused.to_h)
    assert_raises(FsimgSpec::HarnessError) { FsimgSpec::Observation.agrees.excused_by("t.exception") }
    assert_raises(FsimgSpec::HarnessError) { FsimgSpec::Observation.new(state: "excused", detail: "x") }
    assert_raises(FsimgSpec::HarnessError) { FsimgSpec::Observation.new(state: "disagrees", detail: "x", exception_id: "t.exception") }
  end
end
