#!/bin/bash
# Dry run: apply.sh on a Chromium checkout at Vanadium's version, zero fuzz. The checkout is
# shallow (one tag per version) and reused between runs; a failed patch can be fixed in place
# there and its new version taken with git diff.
# Usage: check.sh [WORKDIR]   (default: work/chromium)
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-$ROOT/work/chromium}
VERSION=$(grep -m1 -o '[0-9]\+\(\.[0-9]\+\)\{3\}' "$ROOT/vanadium/args.gn")

mkdir -p "$WORK/src"; cd "$WORK/src"
if [ ! -d .git ]; then
  git init -q
  git remote add origin https://chromium.googlesource.com/chromium/src.git
fi
git rev-parse -q --verify "refs/tags/$VERSION" >/dev/null ||
  git fetch -q --depth=1 origin "refs/tags/$VERSION:refs/tags/$VERSION"
git checkout -q -f "refs/tags/$VERSION"
git clean -qfd
"$ROOT/apply.sh"
