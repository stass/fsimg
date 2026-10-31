# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_default_depth_limit_allows_a_256_component_tree
    root = @root
    # A tree as deep as the default depth limit is created and extracted at
    # the default descriptor limit, though each holds one descriptor per level.
    # The host's own limit must allow that many too, and so must removing the
    # tree; macOS shells start at 256, so the test raises its own to 512 while
    # it runs when the hard limit allows it.
    wanted = 512
    soft, hard = Process.getrlimit(:NOFILE)
    if (hard != Process::RLIM_INFINITY) && (hard < wanted)
      skip("depth-limit tree needs RLIMIT_NOFILE #{wanted}, hard limit #{hard}")
    end

    if (soft != Process::RLIM_INFINITY) && (soft < wanted)
      Process.setrlimit(:NOFILE, wanted, hard)
    end

    begin
      work = (root / "depth-limit")
      # The operand, 254 directories and the file make a 256-component path.
      levels = (["d"] * 254)
      deepest = work.join("tree", *levels)
      FileUtils.mkdir_p(deepest)
      (deepest / "leaf.txt").binwrite("leaf\n".b)
      invoke(root: work, arguments: ["cf", "deep.img", "-F", "fat12", "tree"])
      invoke(root: work, arguments: ["xf", "deep.img"])
      assert_equal("leaf\n".b, work.join("deep", "tree", *levels, "leaf.txt").binread)
      (deepest / "d").mkdir
      (deepest / "d/leaf.txt").binwrite("leaf\n".b)
      result = invoke(
        root: work,
        arguments: ["cf", "deeper.img", "-F", "fat12", "tree"],
        expected: 1
      )
      assert_includes(result.stderr, "depth limit 256".b, result.stderr)
      FileUtils.remove_entry(work)
    ensure
      Process.setrlimit(:NOFILE, soft, hard)
    end
  end
end
