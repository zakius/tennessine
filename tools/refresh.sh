#!/bin/bash
# Refresh a patch that no longer applies cleanly: applies it with fuzz to the Chromium tree in
# the current directory, then rewrites it from the result, keeping its header comment.
# The tree must hold everything before the patch and nothing after: run `STOP=1 ./check.sh`,
# which stops at the first failure. Rejected hunks are left as *.rej to port by hand.
# Fuzz can land a hunk in the wrong place (inside another #if, mid-expression): read every
# hunk patch reports with fuzz, fix the tree, then take the fix with the FILE form.
# Usage (in the tree): tools/refresh.sh PATCH
#                      tools/refresh.sh PATCH FILE...   rewrite FILE's part of PATCH from the
#                      tree's git diff (only when no other patch edits FILE)
set -e
patch_file=$(realpath "$1"); shift

if [ $# -gt 0 ]; then
  for f in "$@"; do
    git diff --src-prefix=a/ --dst-prefix=b/ -- "$f" | sed -n '/^--- /,$p' > "$patch_file.part"
    python3 - "$patch_file" "$f" <<'EOF'
import pathlib, re, sys
p, f = pathlib.Path(sys.argv[1]), sys.argv[2]
s, part = p.read_text(), pathlib.Path(sys.argv[1] + '.part').read_text()
start = re.compile(r'^(diff --git a/%s b/%s\n(index .*\n)?)?--- (a/%s|/dev/null)\n' % ((re.escape(f),) * 3), re.M).search(s)
end = re.compile(r'^(diff --git |--- (a/|/dev/null))', re.M).search(s, start.end() + 1)
p.write_text(s[:start.start()] + part + s[end.start() if end else len(s):])
EOF
    rm "$patch_file.part"
  done
  echo "refreshed $f in $patch_file"; exit
fi

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
