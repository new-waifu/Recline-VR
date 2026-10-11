#!/bin/sh
# Cross-builds everything for the Frame (aarch64, glibc 2.39) with Zig, as an
# alternative to building on the device with CMake. Produces the
# same staged layout as the CMake build in $OUT (default: build-aarch64/stage).
#
#   ZIG=/path/to/zig OPENVR_LIB_DIR=../dev/opt/steamvr/bin/linuxarm64 scripts/build-zig.sh
#
# Zig links its own libc++ statically, so the outputs only need glibc.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ZIG=${ZIG:-zig}
TARGET=${TARGET:-aarch64-linux-gnu.2.39}
OPENVR_INC=${OPENVR_INC:-$ROOT/third_party/openvr}
OPENVR_LIB_DIR=${OPENVR_LIB_DIR:-$ROOT/../dev/opt/steamvr/bin/linuxarm64}
OUT=${OUT:-$ROOT/build-aarch64/stage}
CXXFLAGS="-std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -DRECLINE_FRAME -I$ROOT/common -I$OPENVR_INC"

mkdir -p "$OUT/recline/bin/linuxarm64"
cp -r "$ROOT/packaging/recline/." "$OUT/recline/"
cp "$ROOT"/packaging/overlay/*.json "$ROOT"/scripts/install.sh "$ROOT"/scripts/uninstall.sh "$OUT/"
sed 's/@VR_BINARY_PATH_KEY@/binary_path_linux_arm/' "$ROOT/packaging/overlay/recline_overlay.vrmanifest.in" \
  > "$OUT/recline_overlay.vrmanifest"

driver() { # name sources...
  name=$1; shift
  "$ZIG" c++ -target "$TARGET" $CXXFLAGS -I"$ROOT/driver" -fPIC -shared \
    -fvisibility=hidden -fvisibility-inlines-hidden \
    -Wl,--version-script="$ROOT/driver/exports.map" -Wl,-z,defs \
    -o "$OUT/$name/bin/linuxarm64/driver_$name.so" "$@" -lpthread -ldl -lrt
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

# The XRService passthrough shim: a shared lib exporting only
# VR_GetGenericInterface / VRPaths, resolving the real ones via RTLD_NEXT (so it
# does not link libopenvr_api). Ships next to the driver .so.
shim() {
  "$ZIG" c++ -target "$TARGET" $CXXFLAGS -I"$ROOT/driver" -fPIC -shared \
    -fvisibility=hidden -fvisibility-inlines-hidden \
    -Wl,--version-script="$ROOT/xrshim/exports.map" -Wl,-z,defs \
    -o "$OUT/recline/bin/linuxarm64/librecline_xrshim.so" "$@" -lpthread -ldl -lrt
}

SHM="$ROOT/common/recline_shm.cpp"
CLIENT="$ROOT/common/recline_client.cpp"
driver recline "$ROOT/driver/driver_recline.cpp" "$ROOT/common/slot_patch.cpp" \
  "$ROOT/driver/spawn_inject.cpp" "$SHM"
shim "$ROOT/xrshim/recline_xrshim.cpp" "$ROOT/common/slot_patch.cpp" "$SHM"
app recline_overlay -I"$ROOT/overlay" "$ROOT/overlay/recline_overlay.cpp" \
  "$ROOT/overlay/vk_texture.cpp" "$CLIENT" "$SHM"
app reclinectl "$ROOT/cli/reclinectl.cpp" "$CLIENT" "$SHM"
echo "built into $OUT"
