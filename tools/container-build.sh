#!/bin/bash
# Runs inside the build container (tennessine.py build), in the scratch copy. MODE is empty for a
# full build, x64 for a full build of the emulator's APK in its own tree, or resume (see
# tennessine.py). /keys holds the signing key.
set -e
export DEBIAN_FRONTEND=noninteractive
# tools/Containerfile's image has the packages.
[ -f /etc/tennessine-build-image ] ||
  { apt-get update -qq && apt-get install -y -qq sudo git curl bzip2 xz-utils unzip zstd lsb-release file python3 pigz openjdk-21-jdk-headless >/dev/null; }
# The published APKs' signing key (tennessine.py mounts it): a new one would make every
# installed copy refuse the update.
[ -f /keys/test.jks ] && [ -f /keys/local.properties ] || { echo "no signing key in /keys" >&2; exit 1; }
git config --global user.name tennessine-build
git config --global user.email build@localhost
VERSION=$(grep -m1 -o "[0-9]\+\(\.[0-9]\+\)\{3\}" vanadium/args.gn)
# The files the patches and our src/ write keep their time when their content is the same as at
# the last build (tools/keep_mtimes.py), so preparing the tree again recompiles only what changed.
export KEEP_MTIMES="python3 $PWD/tools/keep_mtimes.py" MTIMES=/build/keep-mtimes.json
export TREES="$PWD/chromium/src $PWD/chromium/src/third_party/devtools-frontend/src"
# A previous run left the tree patched: back to the Chromium tag it checked out, which build.sh
# then moves to VERSION. Ignored files (gclient dependencies, out/) stay, so the sync and the
# compile resume from where they were.
if [ "$MODE" != resume ] && [ -d chromium/src/.git ]; then
  $KEEP_MTIMES save $MTIMES $TREES
  git -C chromium/src am --abort 2>/dev/null || true
  git -C chromium/src reset -q --hard
  git -C chromium/src clean -qfd
  # post-sync.sh edits this gclient dependency, which the reset above doesn't reach: left
  # edited, the next sync refuses to move it to a new Chromium's revision.
  git -C chromium/src/third_party/devtools-frontend/src checkout -q -- . 2>/dev/null || true
fi
export LOCAL_TEST_JKS=$(base64 -w0 /keys/local.properties) STORE_TEST_JKS=$(base64 -w0 /keys/test.jks)
if [ "$MODE" != resume ]; then exec ./build.sh; fi
source common.sh
# common.sh takes SCRIPT_DIR from $0, which is in tools/ here.
SCRIPT_DIR=$PWD
set_keys
[ -d depot_tools ] || git clone -q --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
[ -f depot_tools/python3_bin_reldir.txt ] || (cd depot_tools && ./ensure_bootstrap)
export PATH="$PWD/depot_tools:$PATH"
$KEEP_MTIMES save $MTIMES $TREES
cp -r dubna/src/. src/. chromium/src/
$KEEP_MTIMES restore $MTIMES $TREES
cd chromium/src
OUT=out/Default ABI=arm64-v8a LIB=tennessine/sync_lib CPU=arm64
# The sync library again when it is missing or dubna/rust/ changed since.
if [ ! -f $LIB/liboganesson_sync.so ] ||
   [ -n "$(find $SCRIPT_DIR/dubna/rust -path "*/target" -prune -o -newer $LIB/liboganesson_sync.so -print -quit)" ]; then
  $SCRIPT_DIR/tools/build-sync-lib.sh $LIB /build/sync-cache $CPU
fi
mkdir -p out/tmp out/release && rm -f out/tmp/$VERSION-$ABI.apk out/release/$VERSION-$ABI.apk*
set_version_code $OUT/args.gn
build_ours $OUT || exit 1
autoninja ${JOBS:+-j $JOBS} -C $OUT chrome_public_apk
mv $(find $OUT/apks -name "Chrome*.apk") out/tmp/$VERSION-$ABI.apk
export PATH=$PWD/third_party/jdk/current/bin/:$PATH ANDROID_HOME=$PWD/third_party/android_sdk/public
sign_apk out/tmp/$VERSION-$ABI.apk out/release/$VERSION-$ABI.apk
rm -rf $SCRIPT_DIR/keys
