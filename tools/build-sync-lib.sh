#!/bin/bash
# Builds dubna/rust/oganesson_sync (Firefox Sync over application-services, shared with desktop) for Android (arm64, or x64 for the emulator) into
# OUT/liboganesson_sync.so (build.sh: tennessine/sync_lib in the Chromium tree), which
# patches/tennessine/firefox-sync-native-lib.patch packages with the APK. Runs where build.sh runs (Ubuntu with sudo, or the local build container).
#
# Chromium on Android has no NSS, so NSS is built from source with application-services' own
# scripts, at the commit the crate pins, and linked statically. Chromium's bundled NDK has no
# compilers, hence the official NDK. Rust comes from rustup at application-services' version.
# Everything downloaded is cached in CACHE.
# Usage: tools/build-sync-lib.sh OUT CACHE [arm64|x64]   (x64: the emulator build)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
out=$(mkdir -p "$1" && realpath "$1")
cache=$(mkdir -p "$2" && realpath "$2")
crate=$ROOT/dubna/rust/oganesson_sync
api=34  # Vanadium's minSdk
case ${3:-arm64} in
  arm64) arch=arm64 abi=arm64-v8a triple=aarch64-linux-android ;;
  x64) arch=x86_64 abi=x86_64 triple=x86_64-linux-android ;;
  *) echo "unknown arch $3" >&2; exit 1 ;;
esac
TRIPLE=$(echo "$triple" | tr a-z- A-Z_)

ndk_version=r30
ndk_sha1=5107f898313790e449e87eee2183d9a20602dee9
rust_version=1.94.0
as_rev=$(grep -m1 -o 'application-services", rev = "[0-9a-f]*' "$crate/Cargo.toml" | grep -o '[0-9a-f]*$')

sudo apt-get install -y -qq gyp ninja-build make gcc python3 >/dev/null

# NDK
ndk=$cache/android-ndk-$ndk_version
if [ ! -d "$ndk" ]; then
  zip=$cache/android-ndk-$ndk_version-linux.zip
  [ -f "$zip" ] || curl -fsSL -o "$zip" "https://dl.google.com/android/repository/android-ndk-$ndk_version-linux.zip"
  echo "$ndk_sha1  $zip" | sha1sum -c --quiet
  unzip -q "$zip" -d "$cache"
fi
toolchain=$ndk/toolchains/llvm/prebuilt/linux-x86_64

# NSS, static, for the one ABI.
nss=$cache/nss-$as_rev/$abi/nss
if [ ! -d "$nss" ]; then
  as=$cache/application-services-$as_rev
  if [ ! -d "$as" ]; then
    git init -q "$as"
    git -C "$as" fetch -q --depth 1 https://github.com/mozilla/application-services "$as_rev"
    git -C "$as" checkout -q FETCH_HEAD
  fi
  # build-all.sh downloads and patches NSS, then builds every Android ABI: keep one.
  git -C "$as" checkout -q -- libs/build-all-android.sh
  sed -i "s/^TARGET_ARCHS=.*/TARGET_ARCHS=(\"$arch\")/; s/^TARGET_ARCHS_DISTS=.*/TARGET_ARCHS_DISTS=(\"$abi\")/; s/^TARGET_ARCHS_TOOLCHAINS=.*/TARGET_ARCHS_TOOLCHAINS=(\"$triple\")/" \
    "$as/libs/build-all-android.sh"
  (cd "$as/libs" && ANDROID_NDK_HOME=$ndk ANDROID_NDK_ROOT=$ndk ANDROID_NDK_API_VERSION=$api ./build-all.sh android)
  mkdir -p "$(dirname "$nss")"
  cp -r "$as/libs/android/$abi/nss" "$nss"
fi

# Rust
export RUSTUP_HOME=$cache/rustup CARGO_HOME=$cache/cargo
if [ ! -x "$CARGO_HOME/bin/cargo" ]; then
  curl -fsSL https://sh.rustup.rs | sh -s -- -y -q --no-modify-path --profile minimal --default-toolchain "$rust_version"
fi
"$CARGO_HOME/bin/rustup" target add --toolchain "$rust_version" "$triple" >/dev/null

cc=$toolchain/bin/$triple$api-clang
export NSS_DIR=$nss NSS_STATIC=1
export "CC_${triple//-/_}=$cc" "AR_${triple//-/_}=$toolchain/bin/llvm-ar"
export "CARGO_TARGET_${TRIPLE}_LINKER=$cc"
# NSS's certificate code (mozpkix) is C++: link the NDK's C++ runtime in statically, or
# dlopen fails on __gxx_personality_v0.
export CARGO_TARGET_${TRIPLE}_RUSTFLAGS="-C link-arg=-l:libc++_static.a -C link-arg=-l:libc++abi.a"
export CARGO_TARGET_DIR=$cache/target
# For x86_64 Android, nss_build_common links clang's builtins from
# $ANDROID_HOME/ndk/<the NDK version it pins>: give it ours under that name.
"$CARGO_HOME/bin/cargo" +"$rust_version" fetch --locked --manifest-path "$crate/Cargo.toml"
pinned=$(grep -rho 'ANDROID_NDK_VERSION: &str = "[^"]*' "$CARGO_HOME/git/checkouts" | head -1 | grep -o '[0-9.]*$')
if [ -n "$pinned" ]; then
  mkdir -p "$cache/android-home/ndk"
  ln -sfn "$ndk" "$cache/android-home/ndk/$pinned"
  export ANDROID_HOME=$cache/android-home
fi
"$CARGO_HOME/bin/cargo" +"$rust_version" build --release --locked \
  --target "$triple" --manifest-path "$crate/Cargo.toml"
"$toolchain/bin/llvm-strip" --strip-unneeded -o "$out/liboganesson_sync.so" \
  "$CARGO_TARGET_DIR/$triple/release/liboganesson_sync.so"
echo "built $out/liboganesson_sync.so"
