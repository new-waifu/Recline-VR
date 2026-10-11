#!/bin/sh
# Builds Recline Recenter on the Frame and installs it for the current user:
#
#   ./install.sh        then restart SteamVR
#
# Re-run after pulling updates. BUILD (default: build) is the CMake build dir;
# DEST is passed through to the staged install.sh.
set -eu
ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-$ROOT/build}

if [ ! -x /opt/steamvr/bin/linuxarm64/vrpathreg ]; then
  echo "SteamVR for linuxarm64 not found in /opt/steamvr: run this on the Steam Frame." >&2
  exit 1
fi

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$BUILD" -j "$(nproc)"
ctest --test-dir "$BUILD" --output-on-failure
"$BUILD/stage/install.sh"
