#!/bin/bash
# Runs inside the build container (tennessine.py build), in the scratch copy. MODE is empty for a
# full build, resume or x64 (see tennessine.py). A local test keystore in /keys stands in for
# signing secrets.
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
VERSION=$(grep -m1 -o "[0-9]\+\(\.[0-9]\+\)\{3\}" vanadium/args.gn)
# A previous run left the tree patched: back to the Chromium tag it checked out, which build.sh
# then moves to VERSION. Ignored files (gclient dependencies, out/) stay, so the sync and the
# compile resume from where they were.
if [ "$MODE" != resume ] && [ "$MODE" != x64 ] && [ -d chromium/src/.git ]; then
  git -C chromium/src am --abort 2>/dev/null || true
  git -C chromium/src reset -q --hard
  git -C chromium/src clean -qfd
fi
export LOCAL_TEST_JKS=$(base64 -w0 /keys/local.properties) STORE_TEST_JKS=$(base64 -w0 /keys/test.jks)
if [ "$MODE" != resume ] && [ "$MODE" != x64 ]; then exec ./build.sh; fi
source common.sh
# common.sh takes SCRIPT_DIR from $0, which is in tools/ here.
SCRIPT_DIR=$PWD
set_keys
[ -d depot_tools ] || git clone -q --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
[ -f depot_tools/python3_bin_reldir.txt ] || (cd depot_tools && ./ensure_bootstrap)
export PATH="$PWD/depot_tools:$PATH"
cp -r dubna/src/. src/. chromium/src/
cd chromium/src
OUT=out/Default ABI=arm64-v8a LIB=tennessine/sync_lib CPU=arm64
[ "$MODE" = x64 ] && OUT=out/x64 ABI=x86_64 LIB=tennessine/sync_lib/x64 CPU=x64
# The sync library again when it is missing or dubna/rust/ changed since.
if [ ! -f $LIB/liboganesson_sync.so ] ||
   [ -n "$(find $SCRIPT_DIR/dubna/rust -path "*/target" -prune -o -newer $LIB/liboganesson_sync.so -print -quit)" ]; then
  $SCRIPT_DIR/tools/build-sync-lib.sh $LIB /build/sync-cache $CPU
fi
if [ "$MODE" = x64 ]; then
  mkdir -p $OUT
  sed "s/^target_cpu = .*/target_cpu = \"x64\"/" $SCRIPT_DIR/args.gn > $OUT/args.gn.new
  cmp -s $OUT/args.gn.new $OUT/args.gn || { mv $OUT/args.gn.new $OUT/args.gn; gn gen $OUT; }
fi
mkdir -p out/tmp out/release && rm -f out/tmp/$VERSION-$ABI.apk out/release/$VERSION-$ABI.apk*
set_version_code $OUT/args.gn
autoninja ${JOBS:+-j $JOBS} -C $OUT chrome_public_apk
mv $(find $OUT/apks -name "Chrome*.apk") out/tmp/$VERSION-$ABI.apk
export PATH=$PWD/third_party/jdk/current/bin/:$PATH ANDROID_HOME=$PWD/third_party/android_sdk/public
sign_apk out/tmp/$VERSION-$ABI.apk out/release/$VERSION-$ABI.apk
rm -rf $SCRIPT_DIR/keys
