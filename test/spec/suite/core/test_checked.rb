# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
# frozen_string_literal: true

require "minitest"
require "fsimg_spec"

# Data#with skips initialize on Ruby 3.2, the floor, so every harness
# Data class whose initialize checks or normalizes its fields must
# include Checked.
class TestChecked < Minitest::Test
  def test_every_harness_data_class_with_its_own_initialize_includes_checked
    unchecked = data_classes(FsimgSpec)
                .reject { _1.instance_method(:initialize).owner == Data }
                .reject { _1.include?(FsimgSpec::Checked) }

    assert_empty unchecked.map(&:name)
  end

  def test_with_on_a_checked_class_runs_its_initialize
    assert_raises(FsimgSpec::Error) { FsimgSpec::Status.new(name: "FSIMG_EFORMAT").with(name: "FSIMG_EBOGUS") }
  end

  private

  # The Data classes defined in namespace and the harness modules and
  # classes nested in it.
  def data_classes(namespace)
    nested = namespace.constants(false).map { namespace.const_get(_1) }.grep(Module)
                      .select { _1.name&.start_with?("#{namespace.name}::") }
    nested.select { _1 < Data } + nested.flat_map { data_classes(_1) }
  end
end
