# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  RootPlanCase = Data.define(:name, :policy, :setting, :expected)
  ROOT_PLAN_CASES = [
    RootPlanCase.new(name: "default automatic", policy: [], setting: [], expected: 512),
    RootPlanCase.new(name: "minimized automatic", policy: ["-M"], setting: [], expected: 16),
    RootPlanCase.new(name: "default explicit", policy: [], setting: ["-o", "root=100"], expected: 112),
    RootPlanCase.new(name: "minimized explicit", policy: ["-M"], setting: ["-o", "root=100"], expected: 112)
  ].freeze

  def test_fixed_root_plans_report_rounded_capacity_under_both_policies
    %w[fat12 fat16].each do |format|
      ROOT_PLAN_CASES.each do |plan_case|
        arguments = ["c", "-n", "-R", "-S", "off", "-F", format, *plan_case.policy,
          *plan_case.setting, "-C", "source", "."]
        human = invoke(arguments: arguments)
        assert_includes(human.stdout.lines, "Root entries: #{plan_case.expected}\n".b,
          "#{format} #{plan_case.name}")
        plan = completed(invoke(arguments: ["c", "--json", *arguments.drop(1)]))
          .find { _1.fetch("type") == "plan" }
        assert_equal(plan_case.expected.to_s, plan.fetch("root_entries"),
          "#{format} #{plan_case.name}")
      end
    end
  end

  def test_creation_help_lists_root_setting
    result = invoke(arguments: ["c", "-h"])
    assert_match(/-o key=value\s+Setting:[^\n]*\n\s+reserve, mountpoint or root,/, result.stdout)
  end

  def test_formats_without_a_fixed_root_report_zero_only_in_json
    %w[fat32 exfat qnx6 qnx4 ifs].each do |format|
      arguments = ["c", "-n", "-R", "-S", "off", "-F", format, "-C", "source", "."]
      refute_includes(invoke(arguments: arguments).stdout, "Root entries:".b, format)
      plan = completed(invoke(arguments: ["c", "--json", *arguments.drop(1)]))
        .find { _1.fetch("type") == "plan" }
      assert_equal("0", plan.fetch("root_entries"), format)
    end
  end

  def test_root_count_syntax_rejects_zero_signs_units_and_duplicate_keys
    %w[0 -1 65536 abc 16k].each do |value|
      result = invoke(arguments: ["c", "-n", "-F", "fat12", "-o", "root=#{value}",
        "-C", "source", "."], expected: 2)
      assert_includes(result.stderr, "setting requires a positive decimal count in range".b)
      assert_includes(result.stderr, "root=#{value}".b)
    end
    result = invoke(arguments: ["c", "-n", "-F", "fat12", "-o", "root=16",
      "-o", "root=32", "-C", "source", "."], expected: 2)
    assert_includes(result.stderr, "setting key is given more than once".b)
    assert_includes(result.stderr, "root=32".b)
    result = invoke(arguments: ["c", "-n", "-F", "fat12", "-o", "root=",
      "-C", "source", "."], expected: 2)
    assert_includes(result.stderr, "root=".b)
  end

  def test_root_setting_rejects_inapplicable_formats_and_read_commands
    %w[fat32 exfat qnx6 qnx4 ifs].each do |format|
      result = invoke(arguments: ["c", "-n", "-S", "off", "-F", format,
        "-o", "root=16", "-C", "source", "."], expected: 1)
      assert_includes(result.stderr, "write setting does not apply to the image format".b)
      assert_includes(result.stderr, '"root"'.b)
    end
    %w[t x].each do |mode|
      result = invoke(arguments: [mode, "-o", "root=16", "-f", "unused.img"], expected: 2)
      assert_includes(result.stderr, "setting applies only to creation".b)
      assert_includes(result.stderr, "root=16".b)
    end
  end

  def test_root_rounding_and_insufficient_capacity_fail_without_publishing
    result = invoke(arguments: ["c", "-S", "off", "-F", "fat16", "-o", "root=65535",
      "-f", "overflow.img", "-C", "source", "."], expected: 1)
    assert_includes(result.stderr, "root directory capacity exceeds 65535".b)
    refute((@root / "overflow.img").exist?)

    source = @root / "wide-root"
    source.mkdir
    17.times { (source / "F#{_1.to_s.rjust(7, '0')}").binwrite("".b) }
    [[], ["-M"]].each do |policy|
      result = invoke(arguments: ["c", *policy, "-S", "off", "-F", "fat12",
        "-o", "root=16", "-f", "full.img", "-C", "wide-root", "."], expected: 1)
      assert_includes(result.stderr,
        "FAT root directory capacity 16 is below the 19 entries the root needs".b)
      refute((@root / "full.img").exist?)
    end
  end

  def test_created_images_store_rounded_root_capacity_in_the_bpb
    %w[fat12 fat16].each do |format|
      [[], ["-M"]].each do |policy|
        image = "root-#{format}-#{policy.empty? ? 'default' : 'minimized'}.img"
        invoke(arguments: ["c", *policy, "-R", "-S", "off", "-F", format,
          "-o", "root=100", "-f", image, "-C", "source", "."])
        assert_equal(112, File.binread(@root / image, 2, 17).unpack1("v"), image)
      end
    end
  end
end
