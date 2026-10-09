#!/bin/bash
# Local arm64 build in a rootless Ubuntu container, on a scratch copy in work/build. A local
# test keystore stands in for signing secrets. Output: work/build/tennessine/chromium/src/out/release/*.apk
# `local-build.sh resume` skips the preparation: it copies src/ over the prepared tree, then
# compiles and signs. Enough when only src/ changed; patches or the series need a full run.
# `local-build.sh x64` does the same for an x86_64 APK in out/x64, for the emulator: its ARM
# translation (libndk_translation) crashes arm64 Chromium at startup.
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD=$ROOT/work/build
mkdir -p "$BUILD/tennessine" "$ROOT/work/keys"
rsync -a --delete --exclude=/work --exclude=/oganesson/work --exclude=/oganesson/rust/oganesson_sync/target --exclude=/chromium --exclude=/keys --exclude=/depot_tools "$ROOT/" "$BUILD/tennessine/"
exec podman run --rm -i --pids-limit=-1 -v "$BUILD:/build" -v "$ROOT/work/keys:/keys" -w /build/tennessine \
  -e LOCAL_BUILD=1 -e MODE="${1:-}" -e JOBS="${JOBS:-28}" docker.io/library/ubuntu:24.04 bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq sudo git curl bzip2 xz-utils unzip zstd lsb-release file python3 openjdk-21-jdk-headless >/dev/null
if [ ! -f /keys/test.jks ]; then
  keytool -genkeypair -keystore /keys/test.jks -alias tennessine -keyalg RSA -keysize 4096 \
    -validity 10000 -storepass tennessine -keypass tennessine -dname "CN=tennessine local"
  printf "storePassword=tennessine\nkeyPassword=tennessine\nkeyAlias=tennessine\n" > /keys/local.properties
fi
git config --global user.name tennessine-build
git config --global user.email build@localhost
# A previous run left the tree patched: back to the Chromium tag. Ignored files (gclient
# dependencies, out/) stay, so the sync and the compile resume from where they were.
if [ "$MODE" != resume ] && [ "$MODE" != x64 ] && [ -d chromium/src/.git ]; then
  git -C chromium/src am --abort 2>/dev/null || true
  git -C chromium/src reset -q --hard "chromium_$(grep -m1 -o "[0-9]\+\(\.[0-9]\+\)\{3\}" vanadium/args.gn)"
  git -C chromium/src clean -qfd
fi
export LOCAL_TEST_JKS=$(base64 -w0 /keys/local.properties) STORE_TEST_JKS=$(base64 -w0 /keys/test.jks)
if [ "$MODE" != resume ] && [ "$MODE" != x64 ]; then exec ./build.sh; fi
source common.sh
set_keys
VERSION=$(grep -m1 -o "[0-9]\+\(\.[0-9]\+\)\{3\}" vanadium/args.gn)
[ -d depot_tools ] || git clone -q --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
[ -f depot_tools/python3_bin_reldir.txt ] || (cd depot_tools && ./ensure_bootstrap)
export PATH="$PWD/depot_tools:$PATH"
cp -r src/. chromium/src/
cd chromium/src
OUT=out/Default ABI=arm64-v8a LIB=tennessine/sync_lib CPU=arm64
[ "$MODE" = x64 ] && OUT=out/x64 ABI=x86_64 LIB=tennessine/sync_lib/x64 CPU=x64
# The sync library again when it is missing or oganesson/rust/ changed since.
if [ ! -f $LIB/liboganesson_sync.so ] ||
   [ -n "$(find $SCRIPT_DIR/oganesson/rust -path "*/target" -prune -o -newer $LIB/liboganesson_sync.so -print -quit)" ]; then
  $SCRIPT_DIR/tools/build-sync-lib.sh $LIB /build/sync-cache $CPU
fi
if [ "$MODE" = x64 ]; then
  mkdir -p $OUT
  sed "s/^target_cpu = .*/target_cpu = \"x64\"/" $SCRIPT_DIR/args.gn > $OUT/args.gn.new
  cmp -s $OUT/args.gn.new $OUT/args.gn || { mv $OUT/args.gn.new $OUT/args.gn; gn gen $OUT; }
fi
mkdir -p out/tmp out/release && rm -f out/tmp/$VERSION-$ABI.apk out/release/$VERSION-$ABI.apk*
autoninja ${JOBS:+-j $JOBS} -C $OUT chrome_public_apk
mv $(find $OUT/apks -name "Chrome*.apk") out/tmp/$VERSION-$ABI.apk
export PATH=$PWD/third_party/jdk/current/bin/:$PATH ANDROID_HOME=$PWD/third_party/android_sdk/public
sign_apk out/tmp/$VERSION-$ABI.apk out/release/$VERSION-$ABI.apk
rm -rf $SCRIPT_DIR/keys'
