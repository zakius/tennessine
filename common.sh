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
    # Deflate at level $3 (default 9; 11 is Zopfli) for what's compressed, the native libraries
    # included (tools/repack_apk.py), then aligned again for the stored entries.
    python3 $SCRIPT_DIR/tools/repack_apk.py $1 $1.repacked ${3:-9} || exit 1
    $(dirname $apksigner)/zipalign -f 4 $1.repacked $1.aligned || exit 1
    rm $1.repacked
    $apksigner sign -verbose -ks $SCRIPT_DIR/keys/test.jks --ks-pass pass:$storePassword --key-pass pass:$keyPassword --ks-key-alias $keyAlias --out $2 $1.aligned || exit 1
    rm $1.aligned
}

# Compiles our C++ files' objects in $1 (dubna/tools/ours.py), all of them even after an
# error, so the build stops on one of ours minutes in instead of hours. Run in chromium/src.
build_ours() {
    local ours
    ours=$(python3 $SCRIPT_DIR/dubna/tools/ours.py $1 $SCRIPT_DIR/dubna/src $SCRIPT_DIR/src \
        $(sed "/^#/d;/^$/d;s|^|$SCRIPT_DIR/|" $SCRIPT_DIR/patches/series)) || return 1
    [ -z "$ours" ] || autoninja ${JOBS:+-j $JOBS} -C $1 -k 0 $ours
}
