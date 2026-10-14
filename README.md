# Recline VR

*Lie back and keep playing. Full pitch/roll recentering for SteamVR, built for the
Steam Frame.*

Use VR lying down or reclined. Press one button and the direction you are looking
becomes level and forward in VR, at a chosen eye height. It works for native apps on
the Frame (OpenVR and OpenXR, standing and seated), and every tracked device moves
together, so your hands stay attached. Reset puts everything back.

SteamVR's own recenter can only turn you around (yaw), not tilt the world. Recline
VR rotates the whole tracked world instead; [docs/DESIGN.md](docs/DESIGN.md)
explains how and why.

| Part | Path | What |
|---|---|---|
| `driver_recline.so` | `driver/` | SteamVR driver (adds no devices) that applies the recline rotation to every tracked device's pose. |
| `librecline_xrshim.so` | `xrshim/` | Loaded into the Frame's `XRService` only (never into games); corrects the passthrough mesh and camera poses for the recline rotation. |
| `recline_overlay` | `overlay/` | Dashboard panel, controller actions and haptics; `--register` / `--unregister` for SteamVR. |
| `reclinectl` | `cli/` | Command-line client: status, recenter, reset, diagnostics. |
| shared code | `common/` | `/recline_v1` shared-memory protocol, transform and recenter math. |

## Using it

**Dashboard panel ("Recline"):**

- **Recline recenter** makes your current view level and forward. **Reset to normal**
  undoes it.
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
  the SteamVR dashboard once the dashboard closes; the controller shortcuts keep
  working. To open the panel again, bind *Show the Recline panel* (unbound by
  default) or run `reclinectl panel show`.
  working. To open the panel again, use the *Show the Recline panel* shortcut
  (below) or run `reclinectl panel show`. If that shortcut is unbound, the panel
  warns first and hides on a second click.
- **Recline on SteamVR's recenter when tilted over** (off by default): SteamVR's own
  recenter (system button or menu) then also reclines you when your view is tilted
  more than the set angle (35° by default, 20–75°; pitch and roll both count, so
  lying on your back, side or front all work), and undoes the recline when you
  recenter sitting or standing up. A brief "Reclined" or "Back to normal" in view
  says which happened. *Recline recenter* still works at any angle.
- The status area shows whether the driver is loaded, the current rotation, and
  warnings (another tool hooking poses, boundary limitations).

"Level" means what you see straight through the lenses. On the Frame that is about
14° below the headset's nominal forward axis; the offset is read from SteamVR, so
other headsets work too.

**Controller shortcuts** (default bindings, changeable via the button above). On the
right controller, hold **grip + trigger + bumper** and press **Y** to recenter,
**X** to reset, or **A** to show the panel. They are plain SteamVR chords that work
while a game is running (but not while the dashboard is open).
Defaults are deliberately finger-twisting combinations that normal gameplay won't
produce.

**Safety:** the recline is undone automatically when you sit up, get up, move
away or take the headset off, because SteamVR's boundary is only right without
it. A short "Back to normal" in view tells you it happened.

**Command line** (`~/.local/share/recline-vr/bin/linuxarm64/reclinectl`):

    reclinectl status                      # driver loaded? hook? current rotation, conflicts
    reclinectl recenter [--level-only] [--keep-position] [--height M] [--no-seated-reset]
    reclinectl reset [--no-seated-reset]
    reclinectl set <yaw> <pitch> <roll> <x> <y> <z>   # explicit rotation (deg, m), testing
    reclinectl poses                       # device poses per tracking space + display geometry
    reclinectl bindings                    # open the binding editor in the headset
    reclinectl panel show|hide             # Recline icon in the SteamVR dashboard
    reclinectl setting get|set|remove <section> <key> [value]  # SteamVR settings, for experiments

## Install / uninstall

On the Frame (it ships with CMake and a compiler), with SteamVR running:

    git clone https://github.com/new-waifu/Recline-VR.git
    cd Recline-VR
    ./install.sh        # builds, runs the unit tests, installs to ~/.local/share/recline-vr
    steamvr restart

Everything goes into your home directory; nothing in `/opt` is touched. To update,
`git pull` and run `./install.sh` again, then restart SteamVR the same way.

Uninstall (SteamVR running):

    ~/.local/share/recline-vr/uninstall.sh   # unregisters overlay and driver
    steamvr restart
    rm -rf ~/.local/share/recline-vr         # optionally also /dev/shm/recline_v1

## Build

The OpenVR SDK is vendored in `third_party/openvr`; the overlay and CLI link its
`openvr_api` loader statically. By hand:

    cmake -S . -B build && cmake --build build -j && ctest --test-dir build
    build/stage/install.sh      # installs the staged layout

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

Overlay options are saved in `~/.config/recline-vr/overlay.conf`. The recline
rotation itself is never saved: it is always reset when SteamVR restarts.

## Troubleshooting

The driver logs to `~/.local/share/Steam/logs/vrserver.txt` with prefix `[recline]`:

- `hook installed; chained to …`: working. `hook refused: …` explains why not.
- `GetRawTrackedDevicePoses returns post-world-from-driver poses …`: the conflict
  check is usable; with `… driver-space poses` it disables itself.
- `another pose hook wraps ours …`: another tool (e.g. a calibrator) hooks poses
  and runs before recline, which is the right order for a calibrator.
  `conflict: … modified after our hook …`: something changes poses after recline;
  see docs/DESIGN.md.

If the overlay says the driver isn't loaded, check that SteamVR's safe mode hasn't
disabled it (SteamVR settings, Startup) and that `vrpathreg show` lists the driver.

## Known limitations

- Passthrough (3D and 2D room view) relies on the XRService shim. If it can't load
  or its safety checks fail, passthrough is misaligned while reclined (see
  `~/.local/share/recline-vr/xrshim.log`).
- The boundary is drawn un-rotated and looks tilted while reclined, so Recline
  resets itself as soon as you get up, move away or take the headset off (see
  docs/DESIGN.md). While you lie still, your hands may not trigger the boundary
  where you'd expect.
- A recenter invalidates PC-side tracker calibration (e.g. Space Calibrator) while
  streaming.

## License

MIT; see [LICENSE](LICENSE). Bundled third-party material keeps its own license
(see [THIRD_PARTY.md](THIRD_PARTY.md)).
