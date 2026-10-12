# Recline VR: design notes

How and why Recline VR works the way it does. Observations about SteamVR
internals were made on the Steam Frame's SteamVR build of October 2026. The pose
hook deliberately depends only on the public, versioned OpenVR driver ABI, never
on build-specific addresses or byte patterns. The passthrough fix is the one
exception: it has to use internal (but versioned) client interfaces, and checks
every such assumption at runtime before relying on it (see *Passthrough*).

## Goal

Someone lying down or reclining presses one button, and the direction they are
looking becomes level and forward in VR, at a chosen eye height. This has to work
for native apps on the headset (OpenVR and OpenXR), for games streamed from a PC,
and for every tracked device consistently, so hands stay attached to the body.

## Why a pose hook

The obvious routes don't work:

- **The chaperone API can only yaw.** `IVRChaperoneSetup::SetWorking*ZeroPoseToRawTrackingPose`
  takes a full 3x4 matrix, but SteamVR reduces it to a yaw angle plus translation,
  and `chaperone_info.vrchap` stores only that. Existing playspace tools are limited
  the same way.
- **`TrackingOverrides` can't help.** It can remap one device's pose onto another
  path, but vrserver then discards the real pose of the override *target*, so a
  virtual "rotated head" device can't see the head.

What does work: every driver reports poses through
`IVRServerDriverHost::TrackedDevicePoseUpdated(id, DriverPose_t, size)`, and vrserver
honours `DriverPose_t::qWorldFromDriverRotation` / `vecWorldFromDriverTranslation`
as a general rigid transform, not just a yaw. So if every pose is replaced by
`pose' = R ∘ pose` (only the two world-from-driver fields change), the whole tracked
world rotates. Velocities, accelerations and driver-from-head offsets are in driver
space and follow automatically. Because a constant world transform applies equally
to render-time and display-time poses, it cancels in reprojection and doesn't cause
swim.

Each driver gets its own host object from `GetGenericInterface`, but all host
objects share one vtable inside vrserver, and drivers call through it on every
pose update. Replacing the `TrackedDevicePoseUpdated` slot in that shared vtable
therefore intercepts every driver's poses. `driver_recline` is a server driver
that adds no devices and exists only to install that hook.

## Hook installation and safety

- The slot index comes from the header: the Itanium C++ ABI representation of the
  pointer-to-member `&IVRServerDriverHost::TrackedDevicePoseUpdated` encodes the
  vtable offset (generic and ARM variants are both handled). Nothing is hardcoded.
- Before writing, the slot and its current target are validated against
  `/proc/self/maps`: the slot must sit in a non-writable, file-backed image mapping
  (`r--`, or `r-x` when another hooker left the page that way) and the target in an
  executable one. Otherwise the hook is refused and reported.
- The page is made writable only for an atomic compare-and-swap of the slot, then
  its original protection (read from `/proc/self/maps`) is restored.
- The previous slot value is kept and always called. This chains with hooks
  installed before us and lets later hooks wrap ours.
- The module pins itself with `dlopen(self, RTLD_NODELETE)`, so a `dlclose` can
  never unmap code that might still be in someone's call chain.
- On `Cleanup`, the hook becomes a pure pass-through. If the slot still points at
  our hook it is restored; if someone wrapped us, we stay installed as a
  pass-through rather than break their chain.
- The hot path (about 1 kHz per device) takes no locks, does no allocation and
  doesn't log. Measured cost on the Frame is about 92 ns per HMD pose.
- On Windows (`pose_hook_win.cpp`) the hook is a MinHook detour of the function
  the slot points to rather than a slot patch, because Space Calibrator detours
  that function there: a slot patch would run before any detour whatever the
  install order. MSVC encodes a virtual member pointer as a vcall thunk, so the
  index comes from calling it on a fake object whose vtable entries record their
  position. `VirtualQuery` takes the place of `/proc/self/maps` (image mapping,
  read-only slot, executable target), `GetModuleHandleEx(…PIN)` that of
  `RTLD_NODELETE`. `wrapped` also covers a later detour (our jump at the function
  entry was overwritten), and on `Cleanup` the detour is only disabled if nothing
  wrapped it.
- `tests/test_pose_hook.cpp` checks install, chaining, removal and the ordering
  against a Space Calibrator-style hook installed after ours, with a fake
  `IVRServerDriverHost`: a slot patch on Linux, and on Windows a detour from a
  separate DLL with its own MinHook copy.

## Spaces and recenter math

Rigid transforms are written (q, t) and composed `A ∘ B`.

- **Driver-world:** a pose after the source driver's own world-from-driver
  transform, but before our R.
- **Raw:** what vrserver calls `TrackingUniverseRawAndUncalibrated`, i.e. after R.
- **Standing / seated:** raw transformed by SteamVR's standing or seated zero pose.
  Both zero poses store only yaw and translation.

Head pose in driver-world from a `DriverPose_t d`:

    H.q = d.qWorldFromDriverRotation * d.qRotation * d.qDriverFromHeadRotation
    H.t = d.qWorldFromDriverRotation ⊗ (d.qRotation ⊗ d.vecDriverFromHeadTranslation + d.vecPosition)
          + d.vecWorldFromDriverTranslation

A recenter command carries a target head pose in raw space, `Traw`. The driver
computes `R = Traw ∘ H⁻¹` from its most recent H, so that `R ∘ H = Traw` at that
instant. Doing this in the driver, not the client, avoids any latency mismatch
on H.

The client builds `Traw` without relying on API naming conventions:

1. Read the HMD pose in standing space (`S`) and raw space (`P`).
2. `Z = P ∘ S⁻¹` is raw-from-standing; R cancels out of it.
3. Choose the target standing-space pose: orientation level and forward
   ("Full") or the current orientation minus pitch and roll ("Level only");
   position at the chosen eye height or unchanged ("Keep position").
4. `Traw = Z ∘ S_target`.

**Lens-centre levelling.** "Level" means what the user sees straight through the
lenses, not the head pose's forward axis. On the Steam Frame these differ by about
14.4°: the render frustum is vertically asymmetric (about 50° up, 60° down) and the
lens centre maps to the middle of the render target. The client measures this angle
from `GetProjectionMatrix`, the `LensCenter*` properties and `ComputeDistortion`
(averaged over both eyes), and levels that view axis. On headsets with a centred
frustum the angle is 0 and this is the same as levelling the head axis.

**Seated universe.** Moving the world fixes standing-space apps automatically, but
seated-space apps (and OpenXR `LOCAL`) use the seated zero pose. After a recenter,
and again after a reset, the client optionally calls
`IVRChaperone::ResetZeroPose(TrackingUniverseSeated)` so seated apps are centred on
the user. Recline's own recenter never touches the standing zero pose: it is
persisted and synced to connected PCs. The one exception is the reply to SteamVR's
own recenter (below), which has already reset it.

**SteamVR's own recenter.** The built-in recenter (system button or menu) only
sets a "reset pending" mark for both zero poses. The next OpenVR client to fetch
poses computes the new zero poses (yaw and position only) from the HMD pose it
sees, which is after R, and vrserver persists them and broadcasts
`VREvent_StandingZeroPoseReset` then `VREvent_SeatedZeroPoseReset`. The standing
event has `bResetBySystemMenu` set, but so does the one from an app's
`ResetZeroPose(TrackingUniverseStanding)`: the flag marks a standing reset, not
its source. Recline's own recenter resets only the seated pose, whose event is
unflagged. Measured on the Frame, both events arrive within 10 ms with the new
zero poses already in place.

With *Recline on SteamVR's recenter* on, the overlay answers that flagged event
from the true head pose H (driver-world: gravity-aligned and before R). Its
measure is the tilt of the lens-centre view frame from upright, the angle between
the frame's up axis and world up, so pitch and roll combine and lying on the
back, side or front all count. Over the threshold (35° by default, 20–75°), it
reclines; at or under it, it resets an active recline. Either way it then calls
`ResetZeroPose` for standing and seated space, so SteamVR re-zeroes from the
corrected head:

- Lying down, SteamVR's yaw is meaningless. It comes from the head's forward axis
  projected onto the floor, which is short when the view is near vertical, and on
  the Frame the head axis tips past vertical before the view does (the lens looks
  14° below it). Five presses lying flat gave zero yaws spread over 140°.
  Re-zeroing after the recline uses a level head, so standing forward becomes the
  view direction, whatever the Rotation setting.
- Upright with a recline active, SteamVR has zeroed from the reclined pose, which
  is wrong once R is removed.

Because its own standing reset is flagged too, the overlay ignores flagged
events for a second after making one; it arrives about 40 ms later. Another app
resetting the standing pose is answered like the built-in recenter. Hooking
vrserver's zero-pose handler instead would avoid the brief yaw jump before the
recline lands, but the handler is reached only through internal message
dispatch with no stable layout to check, and a mistake there takes down
vrserver.

Which way the button went depends on the head pose, so the overlay says so: a
small head-locked toast ("Reclined" / "Back to normal") on the lens-centre view
axis, 1 m away so it sits in front of an open dashboard, for about 1.5 s. It is
not shown when nothing changed (upright to upright), nor for Recline's own
recenter and reset, which the user asked for explicitly. `IVRNotifications`
was tried first: the Frame shows those at the edge of view in the Steam
friends/achievements style, which is easy to miss for a change of view.

## Shared-memory protocol `/recline_v1`

`driver_recline` (inside vrserver) and its clients (`recline_overlay`, `reclinectl`)
talk over a 4 KiB POSIX shared-memory segment. It has three blocks, each on its own
64-byte cache line:

- `CmdBlock`: commands (`RECENTER` with `Traw`, `RESET`, `SET` with an explicit R).
  Several clients may write.
- `StateBlock`: acknowledgement, result, flags and current R. Only the driver's
  command thread writes it.
- `HmdBlock`: latest driver-world head pose H, its timestamp, and a pose counter for
  liveness. Written from hook threads.

Every block is a seqlock, and every access to it, sequence word *and* payload, is an
atomic operation, so torn reads are discarded without any data race. Payloads use
relaxed atomics; ordering comes from the sequence word and fences. On the reader
side, an acquire fence before the second sequence read stops payload loads from
moving past it. Only types that are always lock-free are used, since lock-based
atomics don't work across processes.

- **Multi-writer blocks** take the block with a compare-and-swap on the sequence
  word. Command writers spin or yield; hot-path HMD writers skip the publish
  instead of ever spinning.
- **Crashed writers:** a client that dies mid-write leaves the command block's
  sequence odd. The driver treats a sequence that stays at the same odd value for
  over a second as abandoned and releases it. This is the only cross-writer recovery.
- **Hot-path reads of R** never touch shared memory. The driver keeps R in two
  in-process seqlocked copies, written one after the other, so a reader that keeps
  losing races on one copy can always read the other. (A thread-local "last good R"
  would be the textbook fallback, but thread-locals in a `dlopen`ed module can
  allocate on first use per thread.)
- **Initialization:** the creator uses `O_CREAT|O_EXCL`, sizes the segment, writes
  the version and then publishes a magic value with release ordering. Openers wait
  briefly for the magic and refuse a version mismatch. The driver takes over a
  segment whose creator died before publishing the magic. The segment is never
  unlinked while in use; it lives in `/dev/shm` and is cleared on reboot.
- **Handshake:** the client writes a command and remembers its id. The driver's
  ~100 Hz command thread applies it, updating its in-process R first and then the
  state block. The client polls for the matching acknowledgement (500 ms timeout).

## Lifecycle

- R lives only in memory and is identity whenever the driver starts. On start the
  driver also records the current command id, so a command left in shared memory by
  an earlier session is never replayed.
- SteamVR's in-headset "Restart SteamVR" ends vrserver without calling drivers'
  `Cleanup`; a full shutdown does call it. Either way nothing persists, since the
  hook exists only in that process's memory.
- `debugPitchDeg` (driver setting) applies a fixed pitch about the head at startup,
  for testing without any client.

## Dashboard panel

The controller actions belong to the overlay app (`user.recline.overlay`, at
global overlay priority), not to its dashboard overlay, so the panel is optional:
with `showInDashboard=0` the process runs without one and creates it only when
summoned (the *Show the Recline panel* action), until the dashboard next closes.
The panel is never destroyed while on screen (the dashboard is open, or the panel
was detached from it into a game), so hiding it does not pull it out from under
the pointer. `reclinectl panel show|hide` edits `overlay.conf`; the overlay
reloads the file when its modification time changes.

The panel is laser-mouse only. The Frame's dashboard offers a gamepad mode for
panels whose overlay sets flag bit 4 (a gap in the public `VROverlayFlags`): it
gives the panel's process SteamVR Input focus, and a normal-priority action set
then receives the controller. It was tried and dropped: after a flagged panel
had been shown and the dashboard closed, all of the app's action sets stayed
inactive, the global shortcuts included, until a panel without the flag was
opened and closed. Gamepads are no option either: the Frame's SteamVR ships
without the `gamepad` driver, so it has no gamepad input profile.

The Frame's "+" menu was considered as a home for the panel instead. It belongs to
the Steam client, not SteamVR: its "Launch Program" list is the client's
non-Steam-app scan of XDG `.desktop` entries (visibility keys matched against
`XDG_CURRENT_DESKTOP=gamescope`), shown only on the Frame. A desktop entry would
add a file to manage and does nothing on other platforms, so it is not used for now.

## Coexistence with other pose hooks

Other tools (e.g. Space Calibrator-style calibrators) may hook the same slot.

- Every hook modifies its copy of the pose and then calls the previous slot value,
  and the pose is passed by const reference, so the hook installed first applies its
  transform last (outermost). We want to be that hook: a calibrator moving tracked
  devices into the headset's space must run before R, giving `R ∘ Cal ∘ W`. The
  other order, `Cal ∘ R ∘ W`, puts trackers calibrated while upright in the wrong
  place once reclined.
- A driver's `loadPriority` decides `Init` order (higher loads earlier). We use
  100000, so hooks installed by drivers that load after us wrap ours.
- Space Calibrator (the Linux build that runs on the Frame) loads with priority
  9999. At `Init` it patches slot 0 (`GetGenericInterface`) of the driver context
  vtable, which every driver's context shares, and hooks `TrackedDevicePoseUpdated`
  the next time any driver asks for `IVRServerDriverHost_006`. Loading after it, we
  would trigger that hook from our own `Init` and end up beneath it. Its `mprotect`
  also leaves the vtable page `r-x`, which the slot check accepts.
- Hooks from drivers with an even higher priority, or inline detours, can still
  modify poses after us. That can't be prevented without build-specific hooks, so
  the driver reports what it can see:
  - `wrapped` (informational): about once a second, the driver re-reads the slot; if
    it no longer points at our hook, a later hook runs before ours.
  - `conflict_downstream`: about once a second, after forwarding an HMD pose, the
    hook compares `GetRawTrackedDevicePoses` with `R ∘ H`. A persistent mismatch
    means poses are modified after us. On the Frame this API returns post-transform
    poses; if a runtime instead returned driver-space poses, the driver detects that
    and disables the check rather than report false conflicts. Only the HMD is
    checked, so a hook beneath us that moves only trackers goes unnoticed.
- Both are shown in the overlay. The driver never unhooks itself because of them.
- PC-side calibration (e.g. Space Calibrator on a streaming PC) is invalidated by a
  recenter; the overlay says so.

## Passthrough

**The problem.** The Frame's passthrough ("room view") is produced by `XRService`,
a separate process that `driver_cv` launches. It owns the cameras and does the
tracking, and it publishes two poses to the compositor:

- the room-view mesh pose, as the vrpaths property `/meshData/rawFromMeshPose`
  (written with `IVRPaths_002::WritePathBatch`);
- each camera frame's world-from-camera pose, in the frame metadata it hands to
  `IVRCameraPassthroughInternal_001` (which vrclient copies into the shared state
  the compositor reads).

Both are in driver-world, i.e. *before* R, and neither goes through
`TrackedDevicePoseUpdated`. The compositor combines them with the HMD pose, which
does include R, so while reclined the mesh and the camera image are both off by R:
the room appears tilted by the recline angle, and where the image lands on the
wrong geometry it warps.

Both poses are effectively head-relative. Measured live, the mesh pose is
`H_capture ∘ C` and each camera pose `H_capture ∘ E`, where H is the driver-world
head pose and C, E are fixed mount offsets. With both corrected to `R ∘ pose`, the
compositor renders `(R ∘ H_now)⁻¹ ∘ R ∘ H_capture ∘ C = H_now⁻¹ ∘ H_capture ∘ C`.
R cancels, and passthrough looks exactly as it does when not reclined.

**Why the fix lives in XRService.** XRService is statically linked: its only
dynamic imports besides libc and ML runtimes are the OpenVR client entry points.
The poses are computed in stripped code with no seam to interpose, and after
vrclient the compositor reads shared memory directly. The two vrclient calls above
are the narrowest boundary there is, and they run inside XRService.

**Getting a shim into XRService and nowhere else.** Loading an unexpected library
into a game can trip anticheat, so `librecline_xrshim.so` must never reach a game:

- Steam games aren't descendants of vrserver at all (vrserver and Steam are both
  children of `systemd --user`), so vrserver's environment never reaches them. It
  can still launch other programs (vrmanifest apps, overlays), so its global
  environment is never modified.
- `libArcturusPerception.so` (loaded into vrserver by `driver_cv`) spawns XRService
  through `posix_spawnp`, with `posix_spawn` and `execvp` as fallbacks. Once that
  library is loaded, the driver's command thread patches its PLT/GOT slots for
  those three functions (found by symbol name through `dl_iterate_phdr`). The
  wrappers add `LD_PRELOAD=<shim>` only when the program's basename is `XRService`;
  every other spawn passes through untouched. The library is lazily bound, so
  the real functions come from `dlsym(RTLD_NEXT, …)`, not from the slot (which
  holds the PLT resolver until first use). The patch persists across XRService
  restarts.
- The shim does nothing unless `/proc/self/exe` is `…/drivers/cv/bin/linuxarm64/XRService`,
  and removes itself from `LD_PRELOAD` at load so XRService's own children don't
  inherit it.

**What the shim does.** It interposes `VR_GetGenericInterface` and `VRPaths`
(exported by `libopenvr_api.so`; the real ones come from `RTLD_NEXT`) and patches
the vtables of the interfaces it recognises, in place and only after validating
the slot lives in `vrclient.so`. R comes from the driver's `StateBlock`, with
`/recline_v1` mapped read-only.

- *Mesh:* `IVRPaths_002` slot 1 (`WritePathBatch`). XRService writes by path
  handle, so the shim resolves the handle for `/meshData/rawFromMeshPose` through
  slot 2 (`StringToHandle`). It rewrites a matching entry (handle, `HmdMatrix34_t`
  type tag, 48 bytes) on a copy of the batch. The entry layout (stride 0x38) is
  internal, but a wrong layout can't produce that match, so it degrades to a
  pass-through.
- *Camera:* `IVRCameraPassthroughInternal_001` slot 5, `(cameraType, cameraIndex,
  bufferIndex, const info*)`, copies 0xF8 bytes of `info`, with the 3x4
  world-from-camera pose at +0x30. Neither constant is trusted. At install the copy
  size is read back from vrclient's code (the `memcpy` size immediate) and must
  match. Before the first rewrite, the matrix must behave like a head-mounted
  camera: `H⁻¹ ∘ M` has to stay constant per camera, within 25 cm of the head,
  over at least 10° of head motion (H being the driver's pre-R head pose from
  `HmdBlock`). Until then frames pass through uncorrected, so after a SteamVR
  start the correction begins once the head has turned a little. Any mismatch
  disables the camera hook for that XRService run. The rewrite is made on a copy.
- Unknown interface versions are never hooked. Valve bumps these version strings
  when a layout changes, so an update normally just turns the fix off.

**Failure mode.** If the shim can't be injected or a check fails, passthrough is
misaligned while reclined, as it was before. Nothing else is affected. The driver
logs the spawn hook (`spawn injection: N/3`) in `vrserver.txt`; the shim logs
install, validation and first rewrite to `~/.local/share/recline-vr/xrshim.log`,
which is bounded: over 64 KiB it is rotated to `xrshim.previous.log` at the next
XRService start, and one process writes at most 16 KiB.

## Streaming to a PC

On the Frame, the PC-streaming client is an ordinary OpenVR app that reads poses
from the compositor and sends them to the PC, so the Frame-side transform reaches
streamed games with nothing installed on the PC. Verified in use.

## Platforms

OS-specific code sits behind two interfaces with one implementation file per
platform: `common/platform.h` (clock, shared memory, module and executable paths,
dynamic loading, config directory) and `driver/pose_hook.h` (install, remove,
`wrapped`). The CMake platform block picks the files and the toolchain flags.

Everything Frame-specific sits behind the CMake option `RECLINE_FRAME` (compile
definition of the same name), which defaults to on for `linuxarm64` and off
elsewhere:

- the XRService passthrough shim and the driver's spawn injection that loads it;
- the overlay's hint about PC-side calibration while streaming.

The rest is generic. A Linux x86-64 build compiles and links, but hasn't been
tested with a PC headset. The overlay manifest's `binary_path` key is generated
per platform (`binary_path_linux_arm`, `binary_path_linux` or
`binary_path_windows`), and the install scripts find `vrpathreg` in
`/opt/steamvr` or a Steam install (or take `$VRPATHREG`).

A Windows build (MSVC, static CRT) compiles, links and passes the unit tests,
but hasn't been run in SteamVR, and has no install script: the driver would be
registered with `vrpathreg.exe adddriver <stage>\recline` and the overlay with
`recline_overlay.exe --register`. Its shared memory is a session-local named
mapping (`Local\recline_v1`), which disappears with its last user.

## Known limitations

- **Passthrough** is corrected by the XRService shim (verified in both the 3D and
  2D room-view modes). If the shim isn't loaded or one of its checks fails,
  passthrough is misaligned while reclined.
- **Boundaries** are drawn in the un-rotated frame and look tilted while reclined.
  This is deliberately left alone. Someone reclined doesn't move around the
  room, so they don't get near the boundary and it isn't shown. When they get up,
  recentering (or resetting) brings the boundary back in line. Only a moving
  bed or reclining chair would bring it into view while reclined, which is too
  rare to justify another hook into SteamVR internals.
- **SteamVR safe mode** disables add-on drivers after a crash; the overlay reports
  the driver as not loaded.
