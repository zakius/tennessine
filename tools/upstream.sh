#!/bin/bash
# Lists upstream updates for the submodules: a newer Vanadium or Titanium tag, new oganesson
# commits. Prints nothing and exits 0 when everything is current; exits 1 after printing.
# README.md "Updating to a new Chromium" says what to do with each.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
latest_tag() { git ls-remote --tags --refs "$1" | sed 's|.*refs/tags/||' | sort -V | tail -1; }
new=0
for m in vanadium titanium; do
  url=$(git -C "$ROOT" config -f .gitmodules "submodule.$m.url")
  have=$(git -C "$ROOT/$m" describe --tags --exact-match 2>/dev/null || echo "?")
  want=$(latest_tag "$url")
  [ "$have" = "$want" ] || { echo "$m: $have -> $want"; new=1; }
done
have=$(git -C "$ROOT" rev-parse HEAD:oganesson)
want=$(git ls-remote "$(git -C "$ROOT" config -f .gitmodules submodule.oganesson.url)" refs/heads/main | cut -f1)
[ "$have" = "$want" ] || { echo "oganesson: ${have:0:7} -> ${want:0:7}"; new=1; }
exit $new
