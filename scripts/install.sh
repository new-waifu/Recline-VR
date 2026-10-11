#!/bin/sh
# Installs Recline VR for the current user. Run from the
# directory produced by the build (it contains recline/, recline_overlay, ...).
# Only touches $DEST and SteamVR's per-user registries; nothing in /opt.
set -eu
SRC=$(cd "$(dirname "$0")" && pwd)
DEST=${DEST:-$HOME/.local/share/recline-vr}
# SteamVR's vrpathreg: the Frame's fixed location, else a PC Steam install.
if [ -z "${VRPATHREG:-}" ]; then
  VRPATHREG=/opt/steamvr/bin/linuxarm64/vrpathreg
  [ -x "$VRPATHREG" ] || VRPATHREG=$HOME/.steam/steam/steamapps/common/SteamVR/bin/vrpathreg.sh
fi

# Replace each file by rename, never in place: on an update the driver and shim
# are mapped by SteamVR and the overlay binary is running.
put() { # relative path
  mkdir -p "$(dirname "$DEST/$1")"
  cp -p "$SRC/$1" "$DEST/$1.new.$$"
  mv -f "$DEST/$1.new.$$" "$DEST/$1"
}

if [ "$SRC" != "$DEST" ]; then
  (cd "$SRC" && find recline -type f) | while read -r f; do put "$f"; done
  for f in recline_overlay reclinectl recline_overlay.vrmanifest actions.json \
           bindings_frame_controller.json bindings_knuckles.json install.sh uninstall.sh; do
    put "$f"
  done
  # Bundled by builds that don't use the runtime's own copy (off the Frame).
  if [ -f "$SRC/libopenvr_api.so" ]; then put libopenvr_api.so; fi
fi

"$VRPATHREG" adddriver "$DEST/recline"
echo "driver registered: $DEST/recline"

if pgrep -x vrserver >/dev/null 2>&1; then
  "$DEST/recline_overlay" --register
else
  echo "SteamVR is not running: start it and run '$DEST/recline_overlay --register'"
fi

echo "Restart SteamVR, then check ~/.local/share/Steam/logs/vrserver.txt for 'recline'"
echo "and run '$DEST/reclinectl status'."
