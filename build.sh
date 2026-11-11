#!/bin/bash
source common.sh
set_keys
export VERSION=$(grep -m1 -o '[0-9]\+\(\.[0-9]\+\)\{3\}' vanadium/args.gn)
export CHROMIUM_SOURCE=https://chromium.googlesource.com/chromium/src.git # https://github.com/chromium/chromium.git
export DEBIAN_FRONTEND=noninteractive
# tools/Containerfile's image has the packages.
if [ ! -f /etc/tennessine-build-image ]; then
sudo apt-get update
sudo apt-get install -y sudo lsb-release file nano git curl python3 python3-pillow imagemagick librsvg2-bin unzip
sudo dpkg --add-architecture i386; sudo apt-get update; sudo apt-get install -y libgcc-s1:i386
fi

git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH="$PWD/depot_tools:$PATH"
mkdir -p chromium/src/out/Default; cd chromium/src
git init
git remote add origin $CHROMIUM_SOURCE
git fetch --depth 1 $CHROMIUM_SOURCE +refs/tags/$VERSION:chromium_$VERSION
git checkout $VERSION
cp $SCRIPT_DIR/.gclient ../.gclient

python3 $SCRIPT_DIR/tennessine.py apply || exit 1

gclient sync -D --no-history --nohooks || exit 1
gclient runhooks || exit 1
# The image has the dependencies of the install-build-deps it was made with.
cmp -s /tmp/deps/install-build-deps.py build/install-build-deps.py || ./build/install-build-deps.sh --no-prompt

source $SCRIPT_DIR/post-sync.sh
# tools/container-build.sh: unchanged files keep the time they had at the last build.
[ -z "$KEEP_MTIMES" ] || $KEEP_MTIMES restore $MTIMES $TREES
# MODE=x64 (tools/container-build.sh): the emulator's x86_64 APK, in its own tree.
OUT=out/Default ABI=arm64-v8a LIB=tennessine/sync_lib CPU=arm64
[ "$MODE" = x64 ] && OUT=out/x64 ABI=x86_64 LIB=tennessine/sync_lib/x64 CPU=x64
mkdir -p $OUT
sed "s/^target_cpu = .*/target_cpu = \"$CPU\"/" $SCRIPT_DIR/args.gn > $OUT/args.gn
set_version_code $OUT/args.gn
gn gen $OUT # gn args $OUT; echo 'treat_warnings_as_errors = false' >> $OUT/args.gn
rm -rf out/tmp out/release && mkdir -p out/tmp out/release

$SCRIPT_DIR/tools/build-sync-lib.sh $LIB $SCRIPT_DIR/../sync-cache $CPU || exit 1
build_ours $OUT || exit 1
autoninja ${JOBS:+-j $JOBS} -C $OUT chrome_public_apk || exit 1
mv $(find $OUT/apks -name 'Chrome*.apk') out/tmp/$VERSION-$ABI.apk

export PATH=$PWD/third_party/jdk/current/bin/:$PATH
export ANDROID_HOME=$PWD/third_party/android_sdk/public
sign_apk out/tmp/$VERSION-$ABI.apk out/release/$VERSION-$ABI.apk
rm -rf $SCRIPT_DIR/keys
