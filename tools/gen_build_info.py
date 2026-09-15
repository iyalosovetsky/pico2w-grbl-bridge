#!/usr/bin/env python3
"""Generates a small C header with build metadata (version from ../VERSION, timestamp,
git-derived build number/hash/dirty flag) for the startup banner. Re-run on every build
(see CMakeLists.txt's always-run custom target) so it's never stale.

Usage: gen_build_info.py <output .h>
"""
import datetime
import pathlib
import subprocess
import sys


def git(*args, cwd):
    try:
        out = subprocess.run(
            ["git", *args], cwd=cwd, capture_output=True, text=True, timeout=5
        )
        if out.returncode != 0:
            return None
        return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 1

    out_path = sys.argv[1]
    repo_dir = pathlib.Path(__file__).resolve().parent.parent

    version_file = repo_dir / "VERSION"
    version = version_file.read_text().strip() if version_file.exists() else "0.0-unknown"

    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

    count = git("rev-list", "--count", "HEAD", cwd=repo_dir)
    build_number = int(count) if count and count.isdigit() else 0

    git_hash = git("rev-parse", "--short", "HEAD", cwd=repo_dir) or "unknown"

    status = git("status", "--porcelain", cwd=repo_dir)
    dirty = bool(status) if status is not None else False

    with open(out_path, "w") as f:
        f.write("// Auto-generated on every build — do not edit by hand.\n")
        f.write("#pragma once\n\n")
        f.write('#define FIRMWARE_VERSION "{}"\n'.format(version))
        f.write('#define BUILD_TIMESTAMP "{}"\n'.format(timestamp))
        f.write("#define BUILD_NUMBER {}\n".format(build_number))
        f.write('#define BUILD_GIT_HASH "{}"\n'.format(git_hash))
        f.write("#define BUILD_GIT_DIRTY {}\n".format(1 if dirty else 0))

    return 0


if __name__ == "__main__":
    sys.exit(main())
