#!/bin/bash
# Prepare the Chromium tree in the current directory:
# 1. uBlock Origin into third_party/ublock, at the version and checksum in oganesson/deps.ini.
# 2. Firefox Sync's code and strings and no saved history's headers from the oganesson
#    submodule, then src/, copied over the tree.
# 3. Vanadium's patches, straight from the vanadium submodule, renamed to tennessine, minus
#    patches/vanadium-drops.txt.
# 4. patches/series: paths from the repo root, so patches shared with desktop come from the
#    oganesson submodule.
# 5. oganesson's Polish translations of its strings.
# STOP=1 stops at the first failure, for tools/refresh.sh.
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)

ubo() { sed -n '/^\[ublock_origin\]/,/^\[/s/^'"$1"' = //p' "$ROOT/oganesson/deps.ini"; }
UBO_VERSION=$(ubo version)
zip=$ROOT/work/ublock-origin-$UBO_VERSION.zip
mkdir -p "$(dirname "$zip")"
[ -f "$zip" ] || curl -fsSL -o "$zip" \
  "https://github.com/imputnet/uBlock/releases/download/$UBO_VERSION/uBlock0_$UBO_VERSION.chromium.zip"
echo "$(ubo sha256)  $zip" | sha256sum -c --quiet
rm -rf third_party/ublock third_party/uBlock0.chromium
unzip -q "$zip" -d third_party
mv third_party/uBlock0.chromium third_party/ublock

# Firefox Sync's shared code and strings, no saved history's headers, then ours (src/).
cp -r "$ROOT/oganesson/src/chrome/browser/firefox_sync" chrome/browser/
cp "$ROOT/oganesson/src/chrome/app/oganesson_firefox_sync_strings.grdp" chrome/app/
cp "$ROOT/oganesson/src/chrome/browser/download/download_history_saving.h" chrome/browser/download/
cp "$ROOT/oganesson/src/components/sessions/core/closed_tabs_in_memory.h" components/sessions/core/
cp -r "$ROOT/src/." .

dropped() {
  local name=$1 glob why
  while IFS=$'\t' read -r glob why; do
    [[ $glob == \#* || -z $glob ]] && continue
    [[ $name == $glob ]] && return 0
  done < "$ROOT/patches/vanadium-drops.txt"
  return 1
}

fail=0
apply() {  # NAME < patch
  if err=$(git apply --whitespace=nowarn 2>&1); then echo "ok    $1"
  else echo "FAIL  $1"; echo "$err" | sed 's/^/      /'; fail=1; if [ -n "$STOP" ]; then exit 1; fi; fi
}
# A drop that matches nothing: Vanadium renamed or removed the patch.
cut -f1 "$ROOT/patches/vanadium-drops.txt" | grep -v '^#' | while read -r glob; do
  compgen -G "$ROOT/vanadium/patches/$glob" >/dev/null || echo "WARN  no Vanadium patch matches drop $glob"
done
for p in "$ROOT"/vanadium/patches/*.patch; do
  dropped "$(basename "$p")" && continue
  apply "vanadium/${p##*/}" < <(sed 's/VANADIUM/TENNESSINE/g; s/Vanadium/Tennessine/g; s/vanadium/tennessine/g' "$p")
done
while read -r p; do
  apply "$p" < "$ROOT/$p"
done < <(grep -vE '^(#|$)' "$ROOT/patches/series")
# oganesson's translations of its strings (oganesson/l10n), after the patches add the grdp.
python3 "$ROOT/oganesson/tools/translate.py" . || fail=1
exit $fail
