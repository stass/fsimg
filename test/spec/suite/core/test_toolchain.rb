# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "prop_check"

# The toolchain the harness depends on: PropCheck shrinking through Minitest
# assertions, and the header every Ruby source starts with.
class TestToolchain < Minitest::Test
  SPEC_ROOT = File.expand_path("../..", __dir__)
  # The harness's own Ruby; installed gems, such as a vendor/bundle, are not.
  RUBY_SOURCES = "{Gemfile,*.rb,bin/*,lib/**/*.rb,suite/**/*.rb}"
  SPDX_LINE = "# SPDX-License-Identifier: BSD-2-Clause\n"
  FROZEN_LINE = "# frozen_string_literal: true\n"

  # A property that fails for every draw from 500 up. PropCheck must report
  # the Minitest failure, not swallow it, and shrink the failing draw to the
  # boundary itself. All 100 draws from 0..1000 miss the failing half with
  # probability 2**-100.
  def test_propcheck_shrinks_a_failing_assertion_to_its_boundary
    failure = assert_raises(Minitest::Assertion) do
      PropCheck.forall(PropCheck::Generators.choose(0..1000)) { assert_operator _1, :<, 500 }
    end

    assert_equal 500, failure.prop_check_info.fetch(:shrunken_input)
  end

  def test_every_ruby_source_starts_with_the_license_header
    sources = Dir.glob(RUBY_SOURCES, base: SPEC_ROOT)
    refute_empty sources

    missing = sources.reject { license_header?(File.join(SPEC_ROOT, _1)) }
    assert_empty missing, "sources without the SPDX line and frozen_string_literal"
  end

  # The SPDX line comes first; the magic comment belongs to the same leading
  # comment block, the only place Ruby honours it.
  # A program under bin/ starts with its #! line, and the header follows.
  private def license_header?(path)
    leading_comments = File.open(path) { |file| file.each_line.take_while { _1.start_with?("#") } }
    leading_comments = leading_comments.drop(1) if leading_comments.first&.start_with?("#!")
    leading_comments.first == SPDX_LINE && leading_comments.include?(FROZEN_LINE)
  end
end
