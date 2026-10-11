#!/bin/sh
# Uninstalls Recline Recenter. SteamVR must be running for the
# overlay unregistration step.
set -eu
DEST=${DEST:-$HOME/.local/share/recline-vr}
VRPATHREG=/opt/steamvr/bin/linuxarm64/vrpathreg

if pgrep -x vrserver >/dev/null 2>&1; then
  "$DEST/recline_overlay" --unregister
else
  echo "warning: SteamVR is not running; overlay manifest was not unregistered." >&2
  echo "         Start SteamVR and run '$DEST/recline_overlay --unregister' first." >&2
  exit 1
fi

"$VRPATHREG" removedriver "$DEST/recline"
echo "driver unregistered."
echo "Now restart SteamVR so the driver is unloaded, then delete $DEST"
echo "(optionally also: rm /dev/shm/recline_v1)."
