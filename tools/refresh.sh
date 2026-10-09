#!/bin/bash
# Refresh a patch that no longer applies cleanly: applies it with fuzz to the Chromium tree in
# the current directory, then rewrites it from the result, keeping its header comment.
# The tree must hold everything before the patch and nothing after: run `STOP=1 ./check.sh`,
# which stops at the first failure. Rejected hunks are left as *.rej to port by hand.
# Usage (in the tree): tools/refresh.sh PATCH
set -e
patch_file=$(realpath "$1")
files=$(grep -oP '^\+\+\+ b/\K\S+' "$patch_file")
before=$(mktemp -d)
for f in $files; do
  [ -f "$f" ] && mkdir -p "$before/$(dirname "$f")" && cp "$f" "$before/$f"
done
patch -p1 -F3 --no-backup-if-mismatch < "$patch_file"
{
  sed -n '/^\(---\|diff\) /q;p' "$patch_file"
  for f in $files; do
    old=$before/$f label=a/$f
    [ -f "$old" ] || old=/dev/null label=/dev/null
    diff -u --label "$label" --label "b/$f" "$old" "$f" || true
  done
} > "$patch_file.new"
mv "$patch_file.new" "$patch_file"
echo "refreshed $1"
