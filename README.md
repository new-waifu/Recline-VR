# Recline VR

*Lie back and keep playing. Full pitch/roll recentering for SteamVR, built for the
Steam Frame, and also for Windows and Linux PCs.*

Use VR lying down or reclined. Lie back and use SteamVR's own recenter, and the
direction you are looking becomes level and forward in VR, at a chosen eye height.
It works for SteamVR apps (OpenVR and OpenXR, standing and seated), and every
tracked device moves together, so your hands stay attached. Recentering upright
again puts everything back.

SteamVR's own recenter can only turn you around (yaw), not tilt the world. Recline
VR rotates the whole tracked world instead; [docs/DESIGN.md](docs/DESIGN.md)
explains how and why.

| Part | Path | What |
|---|---|---|
| `driver_recline` | `driver/` | SteamVR driver (adds no devices) that applies the recline rotation to every tracked device's pose. |
| `librecline_xrshim.so` | `xrshim/` | Steam Frame only. Loaded into the Frame's `XRService` only (never into games); corrects the passthrough mesh and camera poses for the recline rotation. |
| `recline_overlay` | `overlay/` | Dashboard panel, controller actions and haptics; started by SteamVR. |
| `reclinectl` | `cli/` | Command-line client: status, recenter, reset, overlay registration, diagnostics. |
| shared code | `common/` | `/recline_v1` shared-memory protocol, transform and recenter math. |

## Using it

**Dashboard panel ("Recline"):**

- **Recline on SteamVR's recenter when tilted over** (on by default): SteamVR's own
  recenter (system button or menu) reclines you when your view is tilted more than
  the set angle (35° by default, 20–75°; pitch and roll both count, so lying on your
  back, side or front all work), and undoes the recline when you recenter sitting or
  standing up. A brief "Reclined" or "Back to normal" in view says which happened.
- **Recline recenter** makes your current view level and forward, at any angle.
  **Reset to normal** undoes it.
- **Rotation:** *Full* also turns you to face the play area's forward direction;
  *Level only* keeps the direction you're facing and just straightens the horizon
  (on your back, you end up facing your feet).
- **Position:** *Target height* puts your eyes at the chosen height above the floor
  (presets *Sitting 1.20* and *Standing 1.50*, ±5 cm steps); *Keep position* leaves
  your position alone.
- **Also reset seated zero pose after recenter / reset** (on by default) keeps
  seated-space games, sims and many OpenXR titles centred on you.
- **Edit controller bindings** opens SteamVR's binding editor directly on this app.
- **Show in dashboard** (on by default): untick it to remove the Recline icon from
  the SteamVR dashboard once the dashboard closes; SteamVR's recenter and the panel
  shortcut keep working. To open the panel again, use the *Show the Recline panel*
  shortcut (below) or run `reclinectl panel show`. If that shortcut is unbound, the
  panel warns first and hides on a second click.
- The status area shows whether the driver is loaded and working, what to do if it
  isn't, the current rotation, and warnings (another tool hooking poses, boundary
  limitations).

"Level" means what you see straight through the lenses. On the Steam Frame that is
about 14° below the headset's nominal forward axis; the offset is read from SteamVR,
so other headsets work too.

**Controller shortcut** (default binding, changeable via *Edit controller bindings*):
hold both grips and click both sticks (trackpads if there are no sticks) to show
the panel. It works while a game is running (but not while the dashboard is open).
Recline and reset have no default shortcut, since SteamVR's own recenter does both;
bind *Recline recenter* and *Reset to normal* there if you want dedicated ones.

**Safety:** the recline is undone automatically when you sit up, get up, move
away or take the headset off, because SteamVR's boundary is only right without
it. A short "Back to normal" in view tells you it happened.

**Command line:** `reclinectl` is in the install's `bin/<platform>/` folder
(Steam Frame: `~/.local/share/recline-vr/bin/linuxarm64/reclinectl`; Linux PC:
`~/.local/share/recline-vr/bin/linux64/reclinectl`; Windows:
`%LOCALAPPDATA%\Programs\recline-vr\bin\win64\reclinectl.exe`).

    reclinectl status                      # driver loaded and matching? hook? rotation, conflicts
    reclinectl recenter [--level-only] [--keep-position] [--height M] [--no-seated-reset]
    reclinectl reset [--no-seated-reset]
    reclinectl set <yaw> <pitch> <roll> <x> <y> <z>   # explicit rotation (deg, m), testing
    reclinectl poses                       # device poses per tracking space + display geometry
    reclinectl props                       # SteamVR drivers and device properties, for bug reports
    reclinectl bindings                    # open the binding editor in the headset
    reclinectl panel show|hide             # Recline icon in the SteamVR dashboard
    reclinectl register | unregister       # start the overlay with SteamVR (the installers do this)
    reclinectl setting get|set|remove <section> <key> [value]  # SteamVR settings, for experiments

## Install

### On the Steam Frame

If your headset is a Steam Frame, install Recline VR on the Frame itself. It works
for both standalone and streamed games. The easy way to install is as follows. In
the Frame's desktop mode, with SteamVR running (the Frame ships with CMake and a
compiler):

    git clone https://github.com/new-waifu/Recline-VR.git
    cd Recline-VR
    ./install.sh        # builds, runs the unit tests, installs to ~/.local/share/recline-vr
    steamvr restart

Everything goes into your home directory; nothing in `/opt` is touched. To update,
`git pull` and run `./install.sh` again, then restart SteamVR the same way. A
release download (below) works on the Frame too, without building.

### From a release

Download `recline-vr-<version>.zip` from the
[releases](https://github.com/new-waifu/Recline-VR/releases) (the `-debug.zip` is
only for debugging) and unzip it. It holds every platform. Then:

- **Linux** (a PC, or the Frame): make sure SteamVR is running, run `./install.sh`
  in the unzipped `recline-vr` folder, then restart SteamVR (on the Frame:
  `steamvr restart`). It installs to `~/.local/share/recline-vr`.
- **Windows:** with SteamVR **not** running, double-click `install.cmd` in the
  unzipped `recline-vr` folder, then start SteamVR. It installs to
  `%LOCALAPPDATA%\Programs\recline-vr`. The files aren't code-signed, so Windows
  SmartScreen or your antivirus may warn about them; on SmartScreen's warning,
  choose *More info*, then *Run anyway*.

Either way you can delete the unzipped files afterwards. To update, do the same
with the new release.

**Headsets:** the Steam Frame (installed on the Frame, as above), and wired headsets
on Windows and Linux PCs. Wireless PC headsets aren't supported. Installed on a PC,
Recline VR refuses to recline a headset that streams over Steam Link, Steam Frame
included: the headset re-projects every frame to its own head pose, which undoes
the recline.

### Uninstall

- **Linux and the Steam Frame**, with SteamVR running:

      ~/.local/share/recline-vr/uninstall.sh   # unregisters the overlay and the driver
      steamvr restart                          # on a PC: restart SteamVR
      rm -rf ~/.local/share/recline-vr         # optionally also /dev/shm/recline_v1

- **Windows**, with SteamVR **not** running: run `uninstall.cmd` in
  `%LOCALAPPDATA%\Programs\recline-vr`. It unregisters the overlay and the driver,
  then shows that folder in Explorer for you to delete.

Settings stay behind until you delete them: `~/.config/recline-vr` on Linux,
`%LOCALAPPDATA%\recline-vr` on Windows (which also holds the overlay's log).

To go back to an older version, uninstall first, then install the older one. On
Linux and the Steam Frame, reboot your device in between (or stop SteamVR and
remove `/dev/shm/recline_v1`).

## Build

The OpenVR SDK is vendored in `third_party/openvr`; the overlay and CLI link its
`openvr_api` loader statically. With CMake 3.21 or later (on Windows, Visual
Studio 2019 or later):

    cmake -S . -B build && cmake --build build -j && ctest --test-dir build
    build/stage/install.sh      # installs the staged layout (Windows: build\stage\install.cmd)

The overlay uses Vulkan when its headers are available at build time and falls back
to `SetOverlayRaw` otherwise.

The version (next to the panel title, `reclinectl --version`, and the driver's line
in `vrserver.txt`) comes from `git describe`: `v1.1.0` for a tagged commit,
`v1.1.0+3.14d0169` three commits later, `-dirty` with uncommitted changes.
`-DRECLINE_VERSION=...` overrides it.

## Settings

Driver (`resources/settings/default.vrsettings`, section `driver_recline`):

- `loadPriority` (100000): load before other drivers (including Space Calibrator)
  so their pose hooks wrap ours.
- `debugPitchDeg` (0): apply a fixed pitch about the head at startup, for testing
  without the overlay.
- `downstreamCheck` (true): periodically check that nothing modifies poses after us.

Overlay options are saved in `~/.config/recline-vr/overlay.conf` on Linux and
`%LOCALAPPDATA%\recline-vr\overlay.conf` on Windows. The recline rotation itself is
never saved: it is always reset when SteamVR restarts.

## Troubleshooting

Start with the panel's status line, or `reclinectl status`, which says the same in
more detail: whether the driver is loaded, why not (not installed, turned off in
SteamVR's add-ons, safe mode, failed to load), and whether an update or a second
install needs a restart or an uninstall.

The driver logs to SteamVR's `vrserver.txt` with prefix `[recline]` (in Steam's
`logs` folder: `~/.local/share/Steam/logs` on the Frame and most Linux PCs,
`C:\Program Files (x86)\Steam\logs` on Windows):

- `hook installed; chained to …`: working. `hook refused: …` explains why not.
- `GetRawTrackedDevicePoses returns post-world-from-driver poses …`: the conflict
  check is usable; with `… driver-space poses` it disables itself.
- `another pose hook wraps ours …`: another tool (e.g. a calibrator) hooks poses
  and runs before recline, which is the right order for a calibrator.
  `conflict: … modified after our hook …`: something changes poses after recline;
  see docs/DESIGN.md.

The overlay logs to `overlay.log` (`~/.local/share/recline-vr` on Linux,
`%LOCALAPPDATA%\recline-vr` on Windows; the previous one is kept as
`overlay.previous.log`).

## Known limitations

- On the Steam Frame, passthrough (3D and 2D room view) relies on the XRService
  shim. If it can't load or its safety checks fail, passthrough is misaligned while
  reclined (see `~/.local/share/recline-vr/xrshim.log`).
- The boundary is drawn un-rotated and looks tilted while reclined, so Recline
  resets itself as soon as you get up, move away or take the headset off (see
  docs/DESIGN.md). While you lie still, your hands may not trigger the boundary
  where you'd expect.
- When the Steam Frame streams from a PC, a recline invalidates PC-side tracker
  calibration (e.g. Space Calibrator).

## License

MIT; see [LICENSE](LICENSE). Bundled third-party material keeps its own license
(see [THIRD_PARTY.md](THIRD_PARTY.md)).
