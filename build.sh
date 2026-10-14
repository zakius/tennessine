#!/bin/bash
source common.sh
set_keys
export VERSION=$(grep -m1 -o '[0-9]\+\(\.[0-9]\+\)\{3\}' vanadium/args.gn)
export CHROMIUM_SOURCE=https://chromium.googlesource.com/chromium/src.git # https://github.com/chromium/chromium.git
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update
sudo apt-get install -y sudo lsb-release file nano git curl python3 python3-pillow imagemagick librsvg2-bin unzip
sudo dpkg --add-architecture i386; sudo apt-get update; sudo apt-get install -y libgcc-s1:i386

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
./build/install-build-deps.sh --no-prompt

source $SCRIPT_DIR/post-sync.sh
cp $SCRIPT_DIR/args.gn out/Default/args.gn
set_version_code out/Default/args.gn
gn gen out/Default # gn args out/Default; echo 'treat_warnings_as_errors = false' >> out/Default/args.gn
rm -rf out/tmp out/release && mkdir -p out/tmp out/release

if [ -z "$LOCAL_BUILD" ]; then # local builds: arm64 APK only
autoninja ${JOBS:+-j $JOBS} -C out/Default chrome_public_apk || exit 1
mv $(find out/Default/apks -name 'Chrome*.apk') out/tmp/$VERSION-armeabi-v7a.apk
fi
sed -i 's/target_cpu = "arm"/target_cpu = "arm64"/' out/Default/args.gn
$SCRIPT_DIR/tools/build-sync-lib.sh tennessine/sync_lib $SCRIPT_DIR/../sync-cache || exit 1
autoninja ${JOBS:+-j $JOBS} -C out/Default chrome_public_apk $([ -z "$LOCAL_BUILD" ] && echo chrome_public_bundle) || exit 1
mv $(find out/Default/apks -name 'Chrome*.apk') out/tmp/$VERSION-arm64-v8a.apk
[ -z "$LOCAL_BUILD" ] && mv $(find out/Default/apks -name 'Chrome*.aab') out/tmp/$VERSION-arm64-v8a.aab

export PATH=$PWD/third_party/jdk/current/bin/:$PATH
export ANDROID_HOME=$PWD/third_party/android_sdk/public
[ -z "$LOCAL_BUILD" ] && sign_apk out/tmp/$VERSION-armeabi-v7a.apk out/release/$VERSION-armeabi-v7a.apk
sign_apk out/tmp/$VERSION-arm64-v8a.apk out/release/$VERSION-arm64-v8a.apk
[ -z "$LOCAL_BUILD" ] && sign_aab out/tmp/$VERSION-arm64-v8a.aab out/release/$VERSION-arm64-v8a.aab
rm -rf $SCRIPT_DIR/keys
