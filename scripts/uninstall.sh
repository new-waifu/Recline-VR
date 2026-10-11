#!/bin/sh
# Uninstalls Recline VR. SteamVR must be running for the
# overlay unregistration step.
set -eu
DEST=${DEST:-$HOME/.local/share/recline-vr}
# SteamVR's vrpathreg: the Frame's fixed location, else a PC Steam install.
if [ -z "${VRPATHREG:-}" ]; then
  VRPATHREG=/opt/steamvr/bin/linuxarm64/vrpathreg
  [ -x "$VRPATHREG" ] || VRPATHREG=$HOME/.steam/steam/steamapps/common/SteamVR/bin/vrpathreg.sh
fi

if pgrep -x vrserver >/dev/null 2>&1; then
  "$DEST/recline_overlay" --unregister
else
  echo "warning: SteamVR is not running; overlay manifest was not unregistered." >&2
  echo "         Start SteamVR and run '$DEST/recline_overlay --unregister' first." >&2
  exit 1
fi

"$VRPATHREG" removedriver "$DEST/recline"
echo "driver unregistered."
echo "Now restart SteamVR so the driver is unloaded (on the Frame: steamvr restart),"
echo "then delete $DEST"
echo "(optionally also: rm /dev/shm/recline_v1)."
