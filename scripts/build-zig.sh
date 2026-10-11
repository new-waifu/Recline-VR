#!/bin/sh
# Cross-builds everything for the Frame (aarch64, glibc 2.39) with Zig, as an
# alternative to building on the device with CMake. Produces the
# same staged layout as the CMake build in $OUT (default: build-aarch64/stage).
#
#   ZIG=/path/to/zig VULKAN_INC=/path/to/Vulkan-Headers/include scripts/build-zig.sh
#
# Zig links its own libc++ statically, so the outputs only need glibc.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ZIG=${ZIG:-zig}
TARGET=${TARGET:-aarch64-linux-gnu.2.39}
OVR=$ROOT/third_party/openvr
OUT=${OUT:-$ROOT/build-aarch64/stage}
# Zig doesn't search the host's /usr/include, so the overlay's Vulkan headers
# (header-only use; libvulkan is dlopen'ed) must be given explicitly.
VULKAN_INC=${VULKAN_INC:?set VULKAN_INC to a directory containing vulkan/vulkan.h}
CXXFLAGS="-std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -DRECLINE_FRAME -I$ROOT/common -I$OVR/headers"

mkdir -p "$OUT/recline/bin/linuxarm64"
cp -r "$ROOT/packaging/recline/." "$OUT/recline/"
cp "$ROOT"/packaging/overlay/*.json "$ROOT"/packaging/overlay/icon.png "$ROOT"/scripts/install.sh "$ROOT"/scripts/uninstall.sh "$OUT/"
sed 's/@VR_BINARY_PATH_KEY@/binary_path_linux_arm/' "$ROOT/packaging/overlay/recline_overlay.vrmanifest.in" \
  > "$OUT/recline_overlay.vrmanifest"

driver() { # name sources...
  name=$1; shift
  "$ZIG" c++ -target "$TARGET" $CXXFLAGS -I"$ROOT/driver" -fPIC -shared \
    -fvisibility=hidden -fvisibility-inlines-hidden \
    -Wl,--version-script="$ROOT/driver/exports.map" -Wl,-z,defs \
    -o "$OUT/$name/bin/linuxarm64/driver_$name.so" "$@" -lpthread -ldl -lrt
}
# The openvr_api loader, from the vendored SDK source with the flags its
# CMakeLists uses on linuxarm64; linked statically into the apps.
OVR_OBJ=$ROOT/build-aarch64/openvr
mkdir -p "$OVR_OBJ"
for f in "$OVR"/src/*.cpp "$OVR"/src/vrcore/*.cpp; do
  "$ZIG" c++ -target "$TARGET" -std=c++11 -O2 -fPIC -fvisibility=hidden \
    -DVR_API_PUBLIC -DLINUX -DPOSIX -DLINUXARM64 -DVRCORE_NO_PLATFORM \
    -I"$OVR/src" -I"$OVR/src/vrcore" -I"$OVR/headers" -c "$f" -o "$OVR_OBJ/$(basename "$f" .cpp).o"
done

app() { # name sources...
  name=$1; shift
  "$ZIG" c++ -target "$TARGET" $CXXFLAGS -o "$OUT/$name" "$@" \
    "$OVR_OBJ"/*.o -ldl -lpthread -lrt
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
  "$ROOT/overlay/vk_texture.cpp" "$CLIENT" "$SHM" -DRECLINE_HAVE_VULKAN -isystem "$VULKAN_INC"
app reclinectl "$ROOT/cli/reclinectl.cpp" "$CLIENT" "$SHM"
echo "built into $OUT"
