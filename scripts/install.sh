#!/bin/sh
# Installs Recline VR for the current user from the directory this script is in:
# a build's stage (one platform) or a release package (several platforms).
# Copies the files for this machine to $DEST, which becomes the driver's root,
# and registers the driver and the overlay with SteamVR.
# Only touches $DEST and SteamVR's per-user registries; nothing in /opt.
set -eu
SRC=$(cd "$(dirname "$0")" && pwd)
DEST=${DEST:-$HOME/.local/share/recline-vr}
case $(uname -m) in
  x86_64) PLATFORM=linux64 ;;
  aarch64 | arm64) PLATFORM=linuxarm64 ;;
  *) echo "Recline VR has no build for $(uname -m)" >&2; exit 1 ;;
esac
# SteamVR's vrpathreg: the Frame's fixed location, else a PC Steam install.
if [ -z "${VRPATHREG:-}" ]; then
  VRPATHREG=/opt/steamvr/bin/linuxarm64/vrpathreg
  [ -x "$VRPATHREG" ] || VRPATHREG=$HOME/.steam/steam/steamapps/common/SteamVR/bin/vrpathreg.sh
fi

if [ ! -f "$SRC/bin/$PLATFORM/driver_recline.so" ]; then
  echo "$SRC has no $PLATFORM build (bin/$PLATFORM/driver_recline.so)" >&2
  exit 1
fi

# Replace each file by rename, never in place: on an update the driver and shim
# are mapped by SteamVR and the overlay binary is running.
put() { # relative path
  mkdir -p "$(dirname "$DEST/$1")"
  cp -p "$SRC/$1" "$DEST/$1.new.$$"
  mv -f "$DEST/$1.new.$$" "$DEST/$1"
}

if [ "$SRC" != "$DEST" ]; then
  # Other platforms' bin/ directories stay behind.
  (cd "$SRC" && find "bin/$PLATFORM" resources -type f) | while read -r f; do put "$f"; done
  for f in driver.vrdrivermanifest recline_overlay.vrmanifest VERSION install.sh uninstall.sh \
           LICENSE THIRD_PARTY.md; do
    put "$f"
  done
fi
# Archives and CI artifacts do not always keep the execute bits.
chmod +x "$DEST/bin/$PLATFORM/recline_overlay" "$DEST/bin/$PLATFORM/reclinectl" \
         "$DEST/install.sh" "$DEST/uninstall.sh"

# Up to v1.2 the driver's root was $DEST/recline, with the overlay, reclinectl
# and the overlay's resources directly in $DEST. The overlay manifest kept its
# path, so registering it again below updates it.
if [ -f "$DEST/recline/driver.vrdrivermanifest" ]; then
  "$VRPATHREG" removedriver "$DEST/recline" || true
  rm -rf "$DEST/recline"
  rm -f "$DEST/recline_overlay" "$DEST/reclinectl" "$DEST/actions.json" \
        "$DEST/bindings_frame_controller.json" "$DEST/bindings_knuckles.json" "$DEST/icon.png"
  echo "removed the previous install layout"
fi

"$VRPATHREG" adddriver "$DEST"
echo "driver registered: $DEST"

if pgrep -x vrserver >/dev/null 2>&1; then
  "$DEST/bin/$PLATFORM/reclinectl" register
else
  echo "SteamVR is not running: start it and run '$DEST/bin/$PLATFORM/reclinectl register'"
fi

echo "Restart SteamVR to load this version of Recline VR (on the Frame: steamvr restart)."
