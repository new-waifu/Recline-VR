#!/bin/sh
# Cross-builds everything for the Frame (aarch64, glibc 2.39) with Zig, as an
# alternative to building on the device with CMake (SPEC §6). Produces the
# same staged layout as the CMake build in $OUT (default: build-aarch64/stage).
#
#   ZIG=/path/to/zig OPENVR_LIB_DIR=../dev/opt/steamvr/bin/linuxarm64 scripts/build-zig.sh
#
# Zig links its own libc++ statically, so the outputs only need glibc.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ZIG=${ZIG:-zig}
TARGET=${TARGET:-aarch64-linux-gnu.2.39}
OPENVR_INC=${OPENVR_INC:-$ROOT/../openvr}
OPENVR_LIB_DIR=${OPENVR_LIB_DIR:-$ROOT/../dev/opt/steamvr/bin/linuxarm64}
OUT=${OUT:-$ROOT/build-aarch64/stage}
CXXFLAGS="-std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -I$ROOT/common -I$OPENVR_INC"

mkdir -p "$OUT/recline/bin/linuxarm64"
cp -r "$ROOT/packaging/recline/." "$OUT/recline/"
cp "$ROOT"/packaging/overlay/* "$ROOT"/scripts/install.sh "$ROOT"/scripts/uninstall.sh "$OUT/"

driver() { # name source
  "$ZIG" c++ -target "$TARGET" $CXXFLAGS -I"$ROOT/driver" -fPIC -shared \
    -fvisibility=hidden -fvisibility-inlines-hidden \
    -Wl,--version-script="$ROOT/driver/exports.map" -Wl,-z,defs \
    -o "$OUT/$1/bin/linuxarm64/driver_$1.so" "$2" -lpthread -ldl -lrt
}
# libopenvr_api.so has no SONAME, so linking it directly would record its
# build-host path in DT_NEEDED. Link against a stub with the same exports and
# SONAME libopenvr_api.so instead; the device's real one is found via RPATH.
STUB=$ROOT/build-aarch64/stub
mkdir -p "$STUB"
nm -D --defined-only "$OPENVR_LIB_DIR/libopenvr_api.so" | awk '$2 == "T" { print "void " $3 "(void) {}" }' > "$STUB/stub.c"
"$ZIG" cc -target "$TARGET" -shared -fPIC -Wl,-soname,libopenvr_api.so -o "$STUB/libopenvr_api.so" "$STUB/stub.c"

app() { # name sources...
  name=$1; shift
  "$ZIG" c++ -target "$TARGET" $CXXFLAGS -o "$OUT/$name" "$@" \
    -L"$STUB" -lopenvr_api -Wl,-rpath,/opt/steamvr/bin/linuxarm64 -lpthread -lrt
}

driver recline "$ROOT/driver/driver_recline.cpp"
app recline_overlay -I"$ROOT/overlay" "$ROOT/overlay/recline_overlay.cpp"
app reclinectl "$ROOT/cli/reclinectl.cpp"
echo "built into $OUT"
