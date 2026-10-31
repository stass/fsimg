# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_mtree_summary_reports_publication
    human_image
    root = @root
    # The summary of x --mtree says the manifest was published.
    work = (root / "mtree-reporting")
    work.mkdir
    FileUtils.copy_file((root / "human.img"), (work / "human.img"))
    summary = completed(
      invoke(
        root: work,
        arguments: [
          "xf",
          "human.img",
          "--json",
          "--mtree=json.mtree"
        ]
      )
    )[-1]
    assert_equal(true, summary.fetch("published"), summary)
    assert((work / "json.mtree").exist?, "successful extraction publishes the JSON-requested manifest")
  end
end
