#!/bin/bash
# Runs inside the build container (tennessine.py build), in the scratch copy /build/tennessine.
# One Chromium tree for both APKs: out/Default for arm64, out/x64 (MODE=x64) for the emulator's
# x86_64. PREPARE=1 when the tree isn't prepared for these inputs (tennessine.py compares them
# with the tree's .tennessine-prepared.json); otherwise tennessine.py has copied in any changed
# src/ file.
# VERSION_CODE is the APK's versionCode, RELEASE=1 compresses it with Zopfli. /keys holds the
# signing key.
set -e
# The published APKs' signing key (tennessine.py mounts it): a new one would make every
# installed copy refuse the update.
[ -f /keys/test.jks ] && [ -f /keys/local.properties ] || { echo "no signing key in /keys" >&2; exit 1; }
git config --global user.name tennessine-build
git config --global user.email build@localhost
source common.sh
SCRIPT_DIR=$PWD
# The time each step starts, for finding where a slow build spends it.
step() { echo "[$(date +%T)] $*"; }
VERSION=$(grep -m1 -o "[0-9]\+\(\.[0-9]\+\)\{3\}" vanadium/args.gn)
[ -d depot_tools ] || git clone -q --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
[ -f depot_tools/python3_bin_reldir.txt ] || (cd depot_tools && ./ensure_bootstrap)
export PATH="$PWD/depot_tools:$PATH"

# Back to the Chromium tag (a previous prepare left the tree patched), the patches, gclient's
# dependencies and hooks, and post-sync.sh. Ignored files (gclient dependencies, out/) stay, so
# the sync and the compile resume from where they were. Files whose content comes out as at the
# last prepare get their old time back (tools/keep_mtimes.py), so only what changed recompiles.
prepare() {
  local keep="python3 $SCRIPT_DIR/tools/keep_mtimes.py" state=/build/keep-mtimes.json
  local trees="$PWD/chromium/src $PWD/chromium/src/third_party/devtools-frontend/src $PWD/chromium/src/v8"
  mkdir -p chromium/src
  cd chromium/src
  if [ -d .git ]; then
    step "prepare: record file times"
    $keep save $state $trees
    step "prepare: reset the tree"
    git am --abort 2>/dev/null || true
    git reset -q --hard
    git clean -qfd
    # post-sync.sh edits these gclient dependencies, which the reset above doesn't reach: left
    # edited, the next sync refuses to move them to a new Chromium's revision.
    git -C third_party/devtools-frontend/src checkout -q -- . 2>/dev/null || true
    git -C v8 checkout -q -- . 2>/dev/null || true
  else
    git init -q
    git remote add origin https://chromium.googlesource.com/chromium/src.git
  fi
  step "prepare: fetch and check out $VERSION"
  git fetch -q --depth 1 origin +refs/tags/$VERSION:chromium_$VERSION
  git checkout -q $VERSION
  cp $SCRIPT_DIR/.gclient ../.gclient
  step "prepare: apply the patches"
  python3 $SCRIPT_DIR/tennessine.py apply
  step "prepare: gclient sync and hooks"
  # gclient sync only when what it reads changed: an unchanged sync takes a minute, and moving
  # the dependencies back makes the hooks apply Vanadium's v8 and search engine patches again,
  # which recompiles hundreds of files.
  local synced
  synced=$(find $SCRIPT_DIR/vanadium/subprojects_patches -type f | sort |
    xargs cat $SCRIPT_DIR/.gclient DEPS | sha256sum | cut -d' ' -f1)
  if [ "$(cat /build/synced 2>/dev/null)" != "$synced" ]; then
    rm -f /build/synced
    gclient sync -D --no-history --nohooks
    echo "$synced" > /build/synced
  fi
  gclient runhooks
  # The image has the dependencies of the install-build-deps it was made with.
  cmp -s /tmp/deps/install-build-deps.py build/install-build-deps.py || ./build/install-build-deps.sh --no-prompt
  step "prepare: post-sync.sh"
  source $SCRIPT_DIR/post-sync.sh
  step "prepare: restore file times"
  $keep restore $state $trees
  mv /build/prepared.json.new .tennessine-prepared.json
  cd $SCRIPT_DIR
}

# The arm64 build's PGO profile, which every compile reads. Chromium ships a new one with each
# release (a CIPD package, chrome/build/android-arm64.orderfile.txt), so each release rebuilt
# everything. A milestone keeps the profile of its first build here: a copy in out/pgo, which
# no prepare cleans, with its old time. Prints the GN argument that points at it. The x64
# build's profile already stays the same within a milestone.
pinned_pgo_profile() {
  [ "$CPU" = arm64 ] || return 0
  local pin=out/pgo/arm64-${VERSION%%.*}.profdata
  if [ ! -f $pin ]; then
    mkdir -p out/pgo
    # Under another name first: a copy stopped midway is never taken as whole.
    cp -p chrome/android/orderfiles/arm64/pgo_profile.arm64.profdata $pin.part
    mv $pin.part $pin
  fi
  echo "pgo_data_path = \"//$pin\""
}

[ -z "$PREPARE" ] || prepare
cd chromium/src
OUT=out/Default ABI=arm64-v8a LIB=tennessine/sync_lib CPU=arm64
[ "$MODE" = x64 ] && OUT=out/x64 ABI=x86_64 LIB=tennessine/sync_lib/x64 CPU=x64
mkdir -p $OUT out/tmp out/release
rm -f out/tmp/$VERSION-$ABI.apk out/release/$VERSION-$ABI.apk*
# args.gn changes only when its content does: a new time alone makes gn and parts of the build
# run again.
# args.gn's last line lacks its newline.
{ sed -e "s/^target_cpu = .*/target_cpu = \"$CPU\"/" -e '$a\' $SCRIPT_DIR/args.gn
  echo "android_override_version_code = \"$VERSION_CODE\""
  pinned_pgo_profile; } > $OUT/args.gn.new
if cmp -s $OUT/args.gn.new $OUT/args.gn; then rm $OUT/args.gn.new; else mv $OUT/args.gn.new $OUT/args.gn; fi
step "gn gen"
gn gen $OUT
step "Firefox Sync library"
$SCRIPT_DIR/tools/build-sync-lib.sh $LIB /build/sync-cache $CPU
step "our files"
build_ours $OUT
step "the APK"
autoninja ${JOBS:+-j $JOBS} -C $OUT chrome_public_apk
mv $(find $OUT/apks -name 'Chrome*.apk') out/tmp/$VERSION-$ABI.apk
export PATH=$PWD/third_party/jdk/current/bin/:$PATH ANDROID_HOME=$PWD/third_party/android_sdk/public
export LOCAL_TEST_JKS=$(base64 -w0 /keys/local.properties) STORE_TEST_JKS=$(base64 -w0 /keys/test.jks)
set_keys
step "repack and sign"
sign_apk out/tmp/$VERSION-$ABI.apk out/release/$VERSION-$ABI.apk ${RELEASE:+11}
rm -rf $SCRIPT_DIR/keys
step "done"
