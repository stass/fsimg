# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

module FsimgSpec
  # fsimg's side of the harness contract (specs/architecture.md#specification-driven-testing
  # and specs/design-spec-testing.md#stage-policy): its stages, the checks each
  # stage runs, its status codes and resource names.

  # fsimg_status_t values from include/fsimg.h; the code of a CLI -j error
  # record is one of these.
  STATUS_CODES = {
    "FSIMG_EINVAL" => -1,
    "FSIMG_ESTATE" => -2,
    "FSIMG_EIO" => -3,
    "FSIMG_EFORMAT" => -4,
    "FSIMG_ECORRUPT" => -5,
    "FSIMG_EUNSUPPORTED" => -6,
    "FSIMG_ERANGE" => -7,
    "FSIMG_ELIMIT" => -8,
    "FSIMG_ENOMEM" => -9,
    "FSIMG_ECANCELLED" => -10,
    "FSIMG_ESOURCE_CHANGED" => -11,
    "FSIMG_EEXISTS" => -12,
    "FSIMG_EDURABILITY" => -13
  }.freeze

  # The keys of fsimg -l (cmd/fsimg/options.c).
  LIMIT_KEYS = %w[scratch entries depth path fds output work seconds].freeze

  # The resources an FSIMG_ELIMIT error record names (cmd/fsimg/display.c).
  # The memory budget is -m rather than a -l key, and an expired deadline
  # reports FSIMG_ECANCELLED rather than a limit.
  LIMIT_RESOURCES = %w[memory scratch fds entries output work depth path].freeze

  READING_STAGES = %w[info ls extract verify verify-deep].freeze
  STAGES = (READING_STAGES + %w[create]).freeze
  # The order a case runs its stages in: create writes the image the
  # reading stages then read.
  RUN_ORDER = (%w[create] + READING_STAGES).freeze

  # The check a violation's scope names, and the stages that run it.
  SCOPE_STAGES = {
    "open" => READING_STAGES,
    "directories" => %w[ls extract verify verify-deep],
    "files" => %w[extract verify verify-deep],
    "verification" => %w[verify verify-deep],
    "destination" => %w[extract],
    "input" => %w[create]
  }.transform_values(&:freeze).freeze

  # The statuses image content, creation input or the destination can cause
  # at each stage when fsimg and the host work correctly.
  VERDICT_STATUSES = begin
    reading = %w[FSIMG_EFORMAT FSIMG_ECORRUPT FSIMG_EUNSUPPORTED FSIMG_ERANGE FSIMG_ELIMIT]
    {
      "info" => reading,
      "ls" => reading,
      "extract" => reading + %w[FSIMG_EEXISTS],
      "verify" => reading,
      "verify-deep" => reading,
      "create" => %w[FSIMG_EINVAL FSIMG_ERANGE FSIMG_EUNSUPPORTED FSIMG_ELIMIT]
    }.transform_values(&:freeze).freeze
  end

  # A status name or resource fsimg does not report.
  class StatusError < Error; end

  # A status a rejection reports: an fsimg_status_t name, and for
  # FSIMG_ELIMIT the resource, written FSIMG_ELIMIT:entries.
  class Status < Data.define(:name, :resource)
    include Checked

    def self.parse(text)
      name, resource, *rest = text.split(":", -1)
      raise StatusError, "malformed status #{FsimgSpec.shown(text)}" unless rest.empty?

      new(name:, resource:)
    end

    def initialize(name:, resource: nil)
      raise StatusError, "unknown status #{FsimgSpec.shown(name)}" unless STATUS_CODES.key?(name)
      raise StatusError, "#{name} names no resource" if name != "FSIMG_ELIMIT" && resource
      raise StatusError, "FSIMG_ELIMIT needs one of #{LIMIT_RESOURCES.join(", ")}" if
        name == "FSIMG_ELIMIT" && !LIMIT_RESOURCES.include?(resource)

      super
    end

    def verdict_at?(stage) = VERDICT_STATUSES.fetch(stage).include?(name)

    def to_s = [name, resource].compact.join(":")
  end
end
