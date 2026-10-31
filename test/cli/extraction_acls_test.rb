# SPDX-License-Identifier: BSD-2-Clause
# frozen_string_literal: true
# Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>

class CliTest < Minitest::Test
  def test_extraction_rejects_nontrivial_and_inherited_acls
    human_image
    root = @root
    # A destination with a non-trivial ACL is refused; a fresh directory
    # that inherits one from its parent is removed again and the error names
    # the parent.
    guarded = private_directory(root: root, name: "acl-guarded")
    inheriting = private_directory(root: root, name: "acl-parent")
    if ((grant_acl(path: guarded, inherit: false).status != 0) ||
        (grant_acl(path: inheriting, inherit: true).status != 0))
      skip("extraction ACL checks: the filesystem refuses ACLs")
    end

    result = invoke(arguments: ["xEf", "human.img", "-C", "acl-guarded"], expected: 1)
    assert_includes(result.stderr, "extraction destination carries a non-trivial access control list \"acl-guarded\"".b, "ACL-protected extraction destination")
    assert_empty(guarded.children.to_a, "ACL refusal leaves the guarded destination empty")
    result = invoke(arguments: ["xf", "human.img", "-C", "acl-parent"], expected: 1)
    assert_includes(result.stderr, "-C directory gives new directories a non-trivial access control list \"acl-parent\"".b, "destination parent with an inheritable ACL")
    assert_empty(inheriting.children.to_a, "ACL refusal removes the newly created destination")
    # Every private directory fsimg creates passes the same check: the
    # staging of an --mtree file beside a private destination, a created
    # image's staging, and scratch.
    destination = private_directory(root: root, name: "acl-destination")
    [
      [["xf", "human.img", "--json", "-C", "acl-destination",
        "--mtree=acl-parent/human.mtree"], "--mtree directory"],
      [["cf", "acl-parent/new.img", "-F", "fat12", "--json",
        "-C", "source", "."], "output directory"],
      [["xf", "human.img", "--json", "-C", "acl-destination",
        "-T", "acl-parent"], "scratch directory"]
    ].each do |arguments, noun|
      error = failed(invoke(arguments: arguments, expected: 1))
      assert_equal(
        "#{noun} gives new directories a non-trivial access control list",
        error.fetch("message"),
        error
      )
      assert_equal("acl-parent", error.fetch("path"), error)
      assert_equal("absent", error.fetch("output_state"), error)
      assert_empty(inheriting.children.to_a, arguments)
      assert_empty(destination.children.to_a, arguments)
    end
  end
  private

  def grant_acl(path:, inherit:)
    if RUBY_PLATFORM.include?("darwin")
      rights = "list,search" + (inherit ? ",file_inherit,directory_inherit" : "")
      run_process(["chmod", "+a", "user:nobody allow #{rights}", path.to_s])
    elsif find_program("setfacl")
      run_process(["setfacl", *(inherit ? ["-d"] : []), "-m", "u:nobody:rx", path.to_s],
        stderr: File::NULL)
    else
      skip("extraction ACL checks: no chmod +a or setfacl")
    end
  end
end
