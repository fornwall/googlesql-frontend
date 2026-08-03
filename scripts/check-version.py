#!/usr/bin/env python3
"""Check that release metadata agrees with the frontend's source version."""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read_one_line(name: str) -> str:
    value = (ROOT / name).read_text(encoding="utf-8").strip()
    if not value or "\n" in value:
        raise SystemExit(f"{name} must contain exactly one non-empty line")
    return value


def require(pattern: str, text: str, source: str) -> None:
    if re.search(pattern, text, flags=re.MULTILINE) is None:
        raise SystemExit(f"{source} does not match {pattern!r}")


parser = argparse.ArgumentParser()
parser.add_argument("--binary", type=Path)
parser.add_argument("--tag")
args = parser.parse_args()

version = read_one_line("VERSION")
commit = read_one_line("GOOGLESQL_COMMIT")
if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is None:
    raise SystemExit(f"VERSION is not semantic x.y.z: {version!r}")
if re.fullmatch(r"[0-9a-f]{40}", commit) is None:
    raise SystemExit(f"GOOGLESQL_COMMIT is not a full lowercase SHA: {commit!r}")

header = (ROOT / "frontend/frontend.h").read_text(encoding="utf-8")
require(rf'kVersion\s*=\s*"{re.escape(version)}"', header, "frontend/frontend.h")
require(
    rf'kGoogleSqlCommit\s*=\s*\n?\s*"{re.escape(commit)}"',
    header,
    "frontend/frontend.h",
)

module = (ROOT / "MODULE.bazel").read_text(encoding="utf-8")
require(
    rf'module\(\s*name = "googlesql_frontend",\s*version = "{re.escape(version)}"',
    module,
    "MODULE.bazel",
)
require(rf"googlesql-{re.escape(commit)}", module, "MODULE.bazel")
require(rf"/archive/{re.escape(commit)}\.tar\.gz", module, "MODULE.bazel")

release = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
require(rf"GOOGLESQL_COMMIT:\s*{re.escape(commit)}", release, "release workflow")

if args.binary is not None:
    reported = subprocess.run(
        [args.binary, "--version"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    expected_parts = [f"googlesql-frontend {version}", f"googlesql {commit}"]
    for expected in expected_parts:
        if expected not in reported:
            raise SystemExit(
                f"{args.binary} version output is missing {expected!r}: {reported!r}"
            )

if args.tag is not None and args.tag != f"v{version}":
    raise SystemExit(f"release tag {args.tag!r} must equal 'v{version}'")

print(f"versions agree: googlesql-frontend {version}, googlesql {commit}")
