# Recline Recenter for SteamVR (Steam Frame)

Use VR lying down or reclined. Press one button and the direction you are looking
becomes level and forward in VR, at a chosen eye height. It works for native apps on
the Frame (OpenVR and OpenXR, standing and seated), and every tracked device moves
together, so your hands stay attached. Reset puts everything back.

SteamVR's own recenter can only turn you around (yaw), not tilt the world. Recline
Recenter rotates the whole tracked world instead; [docs/DESIGN.md](docs/DESIGN.md)
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
- The status area shows whether the driver is loaded, the current rotation, and
  warnings (another tool hooking poses, boundary limitations).

"Level" means what you see straight through the lenses. On the Frame that is about
14° below the headset's nominal forward axis; the offset is read from SteamVR, so
other headsets work too.

**Controller shortcuts** (default bindings, changeable via the button above). On the
right controller, hold **grip + trigger + bumper** and press **Y** to recenter, or
**X** to reset. They are plain SteamVR chords that work while a game is running.
Defaults are deliberately finger-twisting combinations that normal gameplay won't
produce.

**Command line** (`~/.local/share/recline-vr/reclinectl`):

    reclinectl status                      # driver loaded? hook? current rotation, conflicts
    reclinectl recenter [--level-only] [--keep-position] [--height M] [--no-seated-reset]
    reclinectl reset [--no-seated-reset]
    reclinectl set <yaw> <pitch> <roll> <x> <y> <z>   # explicit rotation (deg, m), testing
    reclinectl poses                       # device poses per tracking space + display geometry
    reclinectl bindings                    # open the binding editor in the headset
    reclinectl setting get|set|remove <section> <key> [value]  # SteamVR settings, for experiments

## Install / uninstall

On the Frame (it ships with CMake and a compiler), with SteamVR running, from a
clone of this repository:

    ./install.sh        # builds, runs the unit tests, installs to ~/.local/share/recline-vr
    # restart SteamVR, then:
    ~/.local/share/recline-vr/reclinectl status

Everything goes into your home directory; nothing in `/opt` is touched. To update,
pull and run `./install.sh` again, then restart SteamVR.

Uninstall (SteamVR running):

    ~/.local/share/recline-vr/uninstall.sh   # unregisters overlay and driver
    # restart SteamVR, then delete ~/.local/share/recline-vr (optionally /dev/shm/recline_v1)

## Build

The OpenVR SDK headers are vendored in `third_party/openvr`. The overlay and CLI link
the runtime's own `libopenvr_api.so` from `/opt/steamvr`. By hand:

    cmake -S . -B build && cmake --build build -j && ctest --test-dir build
    build/stage/install.sh      # installs the staged layout

Cross-build from a PC with Zig (pins glibc 2.39, links libc++ statically):

    ZIG=/path/to/zig scripts/build-zig.sh        # -> build-aarch64/stage/

x86-64 CMake builds compile the driver and run the unit tests; the overlay and CLI are
skipped unless `OPENVR_LIB_DIR` has a matching `libopenvr_api.so`. The overlay uses
Vulkan when its headers are available at build time and falls back to
`SetOverlayRaw` otherwise.

## Settings

Driver (`recline/resources/settings/default.vrsettings`, section `driver_recline`):

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
- The boundary is drawn un-rotated and looks tilted while reclined. Intentional:
  you don't move around while reclined, so it doesn't show up, and recentering
  when you get up fixes it (see docs/DESIGN.md).
- A recenter invalidates PC-side tracker calibration (e.g. Space Calibrator) while
  streaming.

## Test log (Steam Frame, 2026-10-10)

- **Load:** loads before `driver_cv`, hook installed on vrserver's read-only vtable
  slot, HMD = device 0, ~1024 Hz HMD poses.
- **Recenter (measured with `reclinectl poses`):** standing HMD becomes 0.00±0.01°
  at (0, 1.200, 0); seated becomes identity; reset restores. 63 ms round trip.
- **Full rotation honoured:** `GetRawTrackedDevicePoses` matches the hooked pose
  (0.01 mm / 0.001°); with a 90° pitch, no downstream conflict is flagged.
- **Performance:** `build/bench_hook` ≈ 92 ns per HMD pose.
- **In headset:** world, controllers and overlay look right while reclined (no
  flicker or swim). Overlay clicks, hover, controller long-presses (the earlier
  default bindings) and haptics work.
  `hellovr_vulkan` (standing): Full, Level only and eye height as expected.
  `helloxr --space Local` (seated, OpenXR): with the seated option on, recenter and
  reset both centre the Local origin on the head; with it off the origin stays put.
  (`hellovr_sdl` needs `libGLEW.so.2.1`, absent on the Frame; use
  `/opt/steamvr/tools/hellovr_vulkan_linux`. `helloxr` exits when stdin closes.)
- **Lens offset:** the Frame's lens axis is 14.37° below the head pose's forward axis.
  `reclinectl recenter --head-axis` (diagnostic) levels the head axis instead.
- **Reboot:** driver registration survives; driver and overlay start; rotation starts
  as identity; shutdown logs `hook removed` then `[vrserver] Graceful exit`.
- **Two pose hooks** (a temporary test driver applying a 10° yaw via the same slot),
  with a recline left in shared memory before each restart:
  - test driver loading first: we chain to it; `conflict_downstream` within ~7 s
    (9.99° error), not wrapped.
  - test driver loading after us: it chains to us; `wrapped` within ~1 s,
    downstream check matches (0.02 mm).
  - after every restart the rotation was identity and old commands weren't replayed.
- **SteamVR restart from the headset** ends vrserver without driver cleanup (Valve's
  own driver is skipped too); only full shutdowns log the cleanup. `assert_*.dmp`
  files written at restart come from `vrdashboard` and the Steam client.
- **Passthrough:** while reclined, the room view lines up with the real room in
  both the 3D and the 2D mode (`camera.roomView` 1). The shim is mapped only into XRService (`grep -l librecline_xrshim
  /proc/*/maps`); camera-pose validation passes in about a second of head motion
  (cameras within 6.6 cm of the head, 0.7° spread).
- **Streaming to a PC:** recline works in streamed PC games with nothing installed
  on the PC.
- **Uninstall:** with SteamVR running, `uninstall.sh` unregisters the overlay
  manifest and the driver; after a SteamVR restart nothing loads (no `recline` in
  `vrserver.txt`, no overlay process, shim mapped nowhere). The running overlay
  stays up until that restart. Deleting the install dir and reinstalling with
  `install.sh` restores everything.
- **One-command install:** `./install.sh` in a fresh clone on the Frame builds
  (about 6 s), passes the unit tests and installs over a running copy without
  disturbing SteamVR or the overlay. After a restart the overlay auto-launches from
  the `binary_path_linux_arm`-only manifest, the driver hooks, and the shim loads
  only in XRService.
- **Not yet tested:** the safe-mode message.
