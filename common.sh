export SCRIPT_DIR=$(realpath $(dirname $0))

set_keys() {
    mkdir -p $SCRIPT_DIR/keys
    echo $LOCAL_TEST_JKS | base64 -d > $SCRIPT_DIR/keys/local.properties
    echo $STORE_TEST_JKS | base64 -d > $SCRIPT_DIR/keys/test.jks
    unset LOCAL_TEST_JKS
    unset STORE_TEST_JKS
}

sign_apk() {
    export apksigner=$(find $ANDROID_HOME/build-tools -name apksigner | sort | tail -n 1)
    source $SCRIPT_DIR/keys/local.properties
    # Zopfli for what's compressed, the native libraries included (tools/repack_apk.py), then
    # aligned again for the stored entries.
    python3 $SCRIPT_DIR/tools/repack_apk.py $1 $1.repacked || exit 1
    $(dirname $apksigner)/zipalign -f 4 $1.repacked $1.aligned || exit 1
    rm $1.repacked
    $apksigner sign -verbose -ks $SCRIPT_DIR/keys/test.jks --ks-pass pass:$storePassword --key-pass pass:$keyPassword --ks-key-alias $keyAlias --out $2 $1.aligned || exit 1
    rm $1.aligned
}

# Android's versionCode: 10^9 plus the minutes since 2026-01-01 UTC, so each build updates the
# last one, also on the same Chromium version, and stays above Chromium's own codes (805903974
# for 155.0.8059.39 arm64). Android's ceiling, 2100000000, is about 2000 years away.
set_version_code() {
  # The last line may lack its newline, and the code must go on a line of its own.
  sed -i -e '/^android_override_version_code/d' -e '$a\' "$1"
  echo "android_override_version_code = \"$(( 1000000000 + ($(date +%s) - 1767225600) / 60 ))\"" >> "$1"
}

# Compiles our C++ files' objects in $1 (dubna/tools/ours.py), all of them even after an
# error, so the build stops on one of ours minutes in instead of hours. Run in chromium/src.
build_ours() {
    local ours
    ours=$(python3 $SCRIPT_DIR/dubna/tools/ours.py $1 $SCRIPT_DIR/dubna/src $SCRIPT_DIR/src \
        $(sed "/^#/d;/^$/d;s|^|$SCRIPT_DIR/|" $SCRIPT_DIR/patches/series)) || return 1
    [ -z "$ours" ] || autoninja ${JOBS:+-j $JOBS} -C $1 -k 0 $ours
}
