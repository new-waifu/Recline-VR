# Recline Recenter for SteamVR (Steam Frame)

Implementation of `../SPEC.md`. Press one button while lying down and your
current head orientation becomes level and forward (pitch/yaw/roll 0) at a
chosen origin, for native Frame apps and PC streaming alike.

| Part | Path | What |
|---|---|---|
| `driver_recline.so` | `driver/` | Server driver, no devices. Hooks the shared `IVRServerDriverHost` vtable slot of `TrackedDevicePoseUpdated` and applies `pose' = R ∘ pose` to every device. |
| `recline_overlay` | `overlay/` | Dashboard overlay (software-rendered), global input actions `recenter` / `reset`, haptics, `--register` / `--unregister`. |
| `reclinectl` | `cli/` | `status`, `recenter`, `reset`, `set` over the same protocol. |
| `driver_reclinetest.so` | `testdriver/` | Second vtable hook (fixed yaw/offset) for the §10.6 coexistence test. Not installed by default. |
| protocol / math | `common/` | `/recline_v1` seqlock shared memory, rigid-transform math, §5.3 recenter math. |

## Build

On the Frame (preferred, SPEC §6), from a copy of this dir and `../openvr`:

    cmake -S . -B build && cmake --build build -j && ctest --test-dir build
    # staged, installable layout: build/stage/

Cross-build from WSL with Zig (pins glibc 2.39 exactly; links libc++ statically):

    ZIG=/path/to/zig scripts/build-zig.sh        # -> build-aarch64/stage/

Host (x86-64) CMake builds compile the drivers and run the unit tests; the
overlay/CLI are skipped unless `OPENVR_LIB_DIR` has a matching `libopenvr_api.so`.

## Install / uninstall (SPEC §7)

Copy the staged dir to the device, then with SteamVR running:

    ./install.sh        # copies to ~/.local/share/recline-vr, vrpathreg adddriver, --register
    # restart SteamVR; check vrserver.txt for "recline"; then:
    ~/.local/share/recline-vr/reclinectl status

    ~/.local/share/recline-vr/uninstall.sh   # --unregister, vrpathreg removedriver

## Smoke test without the overlay

Set `driver_recline.debugPitchDeg` to 90 (e.g. in `recline/resources/settings/default.vrsettings`)
and restart SteamVR: the world is pitched 90° about the head once the first HMD pose arrives.

## Driver log lines worth knowing (vrserver.txt, prefix `[recline]`)

- `hook installed; chained to …` / `hook refused: …`
- `GetRawTrackedDevicePoses returns post-world-from-driver poses …` or `… driver-space poses; downstream check disabled`
  answers SPEC §10.5 automatically; the downstream-conflict check disables itself in the second case.
- `conflict: …` when another hook wraps ours or modifies poses after us.

## Deviations from / decisions beyond the spec

- **Hot-path fallback without TLS.** §4.5.4 suggests a thread-local "last good R". A
  `thread_local` in a dlopen'ed module can allocate on first touch per thread, so the
  in-process R is instead kept in two seqlocked copies written one after the other;
  a reader that fails 16 attempts on one copy reads the other. Same protocol, never
  blocks, no allocation. If both fail (practically impossible) that one pose passes
  through untransformed.
- **Vtable index** of `TrackedDevicePoseUpdated` is derived at runtime from the header via
  the Itanium pointer-to-member representation (ARM variant on aarch64), not hardcoded.
- **Shm take-over:** if `/dev/shm/recline_v1` exists but never got its magic (creator
  crashed mid-init), the driver initializes it after ~1 s; clients still refuse.
- **Driver ignores stale commands** written before it started (records the current `cmd_id` at init).
- **Overlay options persist** in `~/.config/recline-vr/overlay.conf` (UI prefs only, never R).
- **Mouse Y:** overlay assumes OpenVR's bottom-left mouse origin; set
  `RECLINE_MOUSE_FLIP_Y=0` if clicks land mirrored (unverified on device).
- **Default bindings:** long-press right `Y` = recenter, right `X` = reset (Frame
  controller); Index: right `B` / `A`. Action set uses overlay-global priority so it
  works in games, which means those long-presses are taken from the game.
- `reclinectl recenter` and `reclinectl reset` also reset the seated zero pose (`--no-seated-reset` to skip), as the overlay does by default: recenter puts seated apps level in front of you, reset puts them back in front of your normal head pose.
- Extra setting `driver_recline.downstreamCheck` (default true) disables the §4.6 raw-pose check.

## Device results (2026-10-10)

- §10.1 load: loads before `driver_cv` (loadPriority 100), hook installed on the
  `r--p` vrserver vtable slot, HMD = device 0, ~1024 Hz HMD poses.
- §10.3 recenter (numeric, via `reclinectl poses`): standing HMD becomes
  0.00±0.01° at (0, 1.200, 0); seated becomes identity; reset restores. 63 ms round trip.
- §10.5: `GetRawTrackedDevicePoses` returns post-world-from-driver poses (0.01 mm /
  0.001° match), so the downstream check is active; with R = pitch 90° no conflict is
  flagged, i.e. vrserver honors the full rotation.
- §10.8 perf: `build/bench_hook` ≈ 92 ns per HMD call (budget 2 µs).
- §10.2 visual (in headset while reclined, user's own apps; hellovr_sdl/helloxr not yet
  run): controllers stay on the hands, no flicker or obvious swim.
- Overlay (Vulkan texture), clicks, hover highlight, controller long-presses and haptics
  work in headset.
- Lens-centre recenter: the Frame's lens axis is ~14.4° below the head pose's forward
  axis (asymmetric frustum); recenter levels the lens axis by default.
- Passthrough is broken while reclined (both 3D and 2D room view); under investigation separately.
- Still open: seated-app check (§10.3, e.g. helloxr), Level only confirmation, streaming (§10.4), coexistence (§10.6),
  restart/reboot (§10.7), safe-mode message, uninstall.
