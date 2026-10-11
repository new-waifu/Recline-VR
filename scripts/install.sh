#!/bin/sh
# Installs Recline Recenter for the current user (SPEC §7). Run from the
# directory produced by the build (it contains recline/, recline_overlay, ...).
# Only touches $DEST and SteamVR's per-user registries; nothing in /opt.
set -eu
SRC=$(cd "$(dirname "$0")" && pwd)
DEST=${DEST:-$HOME/.local/share/recline-vr}
VRPATHREG=/opt/steamvr/bin/linuxarm64/vrpathreg

if [ "$SRC" != "$DEST" ]; then
  mkdir -p "$DEST"
  cp -r "$SRC/recline" "$DEST/"
  for f in recline_overlay reclinectl recline_overlay.vrmanifest actions.json \
           bindings_frame_controller.json bindings_knuckles.json install.sh uninstall.sh; do
    cp "$SRC/$f" "$DEST/"
  done
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
