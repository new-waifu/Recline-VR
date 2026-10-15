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

A driver may leave a rotation it doesn't use all zero; SteamVR's null driver
sends its device rotation (`qRotation`) that way. vrserver multiplies the
world-from-driver, device and driver-from-head rotations and only then reads an
all-zero product as no rotation, so a zero factor would drop R's rotation while its
translation still applied. The hook writes the identity such a zero stands for.

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
64-byte cache line, and an identity block (see Driver identity):

- `CmdBlock`: commands (`RECENTER` with `Traw`, `RESET`, `SET` with an explicit R).
  Several clients may write.
- `StateBlock`: acknowledgement, result, flags, current R, and a counter and reason
  for the latest automatic reset. Only the driver's command thread writes it.
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
- **Initialization:** only the driver creates the segment, so a missing one means
  no driver started. It uses `O_CREAT|O_EXCL`, sizes the segment, writes the layout
  version and then publishes a magic value with release ordering. Openers wait
  briefly for the magic and refuse another layout version. The driver
  takes over a segment whose creator died before publishing the magic, and replaces
  one with another layout version: in `/dev/shm` a segment outlives SteamVR until
  reboot, so after an update the previous version's is still there. It unlinks the
  name and creates a new segment; a client that still maps the old one keeps it and
  sees a driver that no longer runs. On Windows a named mapping goes away with its
  last user, so there is nothing left over, and one still in use is refused.
- **Handshake:** the client writes a command and remembers its id. The driver's
  ~100 Hz command thread applies it, updating its in-process R first and then the
  state block. The client polls for the matching acknowledgement (500 ms timeout).

## Driver identity

The overlay and `reclinectl` must know whether the driver they talk to is their own
build, also when its layout differs from theirs. So part of the segment is frozen:
its name (`/recline_v1`), its size, the magic and layout version at offsets 0 and 4,
and an `IdentityBlock` at offset 2048. A layout change moves anything else, never
these. The driver writes the block once at startup: its version (`RECLINE_VERSION`),
the path it was loaded from, that file's size and modification time, and vrserver's
process id.

Segments start zero-filled and no version before 1.3 wrote past byte 448, so a
segment of an older driver reads as "no identity". 1.3 still raised the layout
version to 2: older overlays and `reclinectl`, which can't check an identity, then
refuse a 1.3 driver instead of using it unchecked.

A client compares the identity with itself, in this order:

- **Left over:** vrserver's process id is gone and no poses arrive: the segment is
  from an earlier session (Linux), and no driver runs (see Not running).
- **Another install:** the driver's directory is not the client's own `bin/<platform>/`.
  Reported whatever the versions: SteamVR loads the first registered driver of a
  name and skips the others ("Skipping duplicate external driver"), so a restart
  would bring the same one back. A driver older than 1.3 can't say where it is
  from; if another Recline install is registered, it is reported as this case too.
- **Another build:** another layout version, or another version string: the
  driver's or the client's install changed while SteamVR ran. "Restart SteamVR".
- **Not running:** no segment, or no identity in it. In order: SteamVR's settings
  (safe mode, `blocked_by_safe_mode`; the add-on turned off, `enable`), then its
  driver registry: `external_drivers` in `openvrpaths.vrpath`, read with the OpenVR
  loader's own reader, which is where `vrpathreg` writes. If the client's install
  isn't there, "run the installer", naming any other folder Recline is registered
  from. Otherwise SteamVR failed to load it or was started before the install:
  "restart SteamVR". In that case `reclinectl status` loads the driver library in a
  child process and prints the system loader's error (a missing library, a glibc
  too new for the system, a file that isn't a library), which otherwise only
  vrserver.txt shows.
- **Updated:** the running driver and the client match, but the driver's file
  changed since it was loaded, or the install's `VERSION` is no longer the
  client's.
- **Also registered:** everything matches, but another Recline install is
  registered too. SteamVR skips it now; removing or re-registering one install
  would bring the other back unannounced, so it is a warning (full trust).

The comparison is exact rather than "compatible": whether two builds would work
together is a guess, while a mismatch only arises from an update or a second
install, and a restart, or removing one install, fixes it. The size and time of the
file catch rebuilds that keep the version string (such as `-dirty` development
builds) without a build id that would rebuild everything on each build.

## Acting on the driver's state

The driver's state reaches the overlay's actions in three places: the automatic
reset counter (re-zero the seated pose, and the standing pose if it was zeroed
while reclined), the active flag and head pose (answer SteamVR's recenter: recline
or reset, then re-zero both poses), and the headset's device index (whose worn
events time both). Zero poses persist in SteamVR's chaperone beyond the session, so
a misread state does lasting harm: a driver of another build may publish the same
layout with another meaning. Every one of those places asks one gate in the overlay
(`Trusted()`, whose comment repeats this rule) for one of three levels:

- **Full** (the identity matches, the install is unchanged, poses arrive): everything.
- **Cleanup** (this build's pair, but newer files are installed): no new recline and
  no answering SteamVR's recenter, but resets, including the automatic one, re-zero
  as usual. Refusing those too would strand a standing pose zeroed while reclined.
- **None** (any other driver, or none): its state is shown, never acted on. Recenter
  is refused. Reset sends a bare `CMD_RESET` without zero poses, which needs nothing
  but an open segment of this layout. The driver's own automatic reset (moved away,
  headset off) still undoes a recline without the overlay.

There is no override. `reclinectl` applies the same levels: `recenter` and `set` need
Full, `reset` re-zeroes the seated pose from Cleanup on, and `status` exits non-zero
below Full. Third-party conditions (another hook wrapping ours, a downstream
conflict) are warnings, not failures: Recline can't fix them by restarting.

## Lifecycle

- R lives only in memory and is identity whenever the driver starts. On start the
  driver also records the current command id, so a command left in shared memory by
  an earlier session is never replayed.
- SteamVR's in-headset "Restart SteamVR" ends vrserver without calling drivers'
  `Cleanup`; a full shutdown does call it. Either way nothing persists, since the
  hook exists only in that process's memory.
- `debugPitchDeg` (driver setting) applies a fixed pitch about the head at startup,
  for testing without any client.

## Automatic reset

SteamVR's boundary can't follow the recline (see Known limitations), so R must
never stay active once the user can walk around. The driver undoes it on its own,
in the command thread, so this doesn't depend on the overlay running:

- **Moved away.** Whenever R becomes non-identity, the driver records the real
  head position (driver-world H, before R) as the anchor. It resets when the head
  is more than 0.4 m away horizontally, or more than 0.35 m higher, for 0.25 s.
  Measured on the Frame, lying on a couch or reclining in a chair while shifting,
  reaching and rolling over stayed within 0.23 m horizontally and 0.26 m up.
  Sitting up and getting out of a chair passed 0.4 m horizontally about a second
  in, before the first step. Getting out of a chair hardly raises the head, so
  horizontal travel is the main signal; an absolute head height would also fail
  for recliners, where a reclined head is about as high as a seated one.
  Tilt is deliberately not used: a shallow recline made on purpose is upright
  enough to look like sitting.
- **Headset taken off.** The headset's proximity sensor
  (`VREvent_TrackedDeviceUserInteractionEnded` / `Started` for the HMD) reports
  "not worn" for about half a second at random while worn, so the driver resets
  only after 3 s without "worn", or when SteamVR enters standby. Otherwise a
  headset taken off while reclined would still be reclined when put on again,
  possibly standing.

The overlay sees the reset counter change. It says why ("Back to normal: you
moved" / "headset was off") with a toast and a haptic pulse, once the headset is
worn again, and re-zeroes the seated pose (and the standing pose, if it was last
zeroed while reclined) in front of the user; it re-zeroes only for a driver it trusts
(see Acting on the driver's state).

## Dashboard panel

The controller actions belong to the overlay app (`user.recline.overlay`, at
global overlay priority), not to its dashboard overlay, so the panel is optional:
with `showInDashboard=0` the process runs without one and creates it only when
summoned (the *Show the Recline panel* action), until the dashboard next closes.
The panel is never destroyed while on screen (the dashboard is open, or the panel
was detached from it into a game), so hiding it does not pull it out from under
the pointer. `reclinectl panel show|hide` edits `overlay.conf`; the overlay
reloads the file when its modification time changes.

With the panel hidden, and for a shortcut used in a game, the panel's status line
is out of sight, so problems also show as a toast. A refused or failed shortcut
always says why, briefly, with no haptic pulse (a pulse means it worked); so does
SteamVR's own recenter when "Recline on SteamVR's recenter" is on, since it then
acts as a shortcut. A problem
the user must act on (restart SteamVR, driver not loaded, two installs) shows once
per condition while the overlay runs, and only while the panel is hidden, once the
headset is worn and the condition has lasted 3 s (the driver may still be starting).
The panel is never shown on its own: hiding it was the user's choice.
`overlay.log` records each change of the status line once.

Only *Show the Recline panel* has a default binding: hold both grips and click both
sticks (trackpads where there are none), the same chord on every controller type
the overlay ships bindings for (`tools/gen_bindings.py` writes them). Recline and
reset come from SteamVR's own recenter instead (*Recline on SteamVR's recenter*, on
by default), which every headset has and which no game uses for anything else, so a
default chord for them would only add a way to trigger them by accident. Users can
still bind them. The chords follow what SteamVR's binding editor writes, learned on
the Frame: sources in `button` mode and chord inputs `single`; a chord input its
source's mode doesn't provide (`click` is the `trigger` mode's) makes SteamVR drop
the whole chord ("Unbound input … for chord, ignoring chord"), and `held` and `long`
fire at once inside a chord, so there is no long-press chord.

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

SteamVR starts the overlay without a terminal, so it logs to `overlay.log` in the
user data directory (`~/.local/share/recline-vr`, `%LOCALAPPDATA%\recline-vr`)
as well as stderr. The log is bounded like the XRService shim's: at startup a log
over 64 KiB becomes `overlay.previous.log`, and one run writes at most 64 KiB.

Steam turns on its Vulkan pipeline-recording layer (Fossilize) for what it
starts, through `ENABLE_VK_LAYER_VALVE_steam_fossilize_1`, and SteamVR passes
that on to the overlay together with SteamVR's own pipeline cache path. The
overlay builds no pipelines, so the layer would only log and add an empty cache
file to SteamVR's; the overlay sets the layer manifest's
`DISABLE_VK_LAYER_VALVE_steam_fossilize_1` before loading Vulkan.

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

Recline VR installed on the PC can't recline a headset that streams over Steam
Link (SteamVR's `vrlink` driver), a streamed Frame or any other. The driver applies
R as usual and the PC renders reclined, but the headset re-projects every frame to
its own head pose, and R is exactly such a difference: tested with a Frame, the view
stayed upright, tilting further showed the black edge of the streamed image, the
area sent sharpest (foveated) no longer matched where the user looked, and the
laser pointer and highlights drifted from what was drawn. So on a Steam Link
headset (`Prop_ActualTrackingSystemName_String` is `vrlink`, "the literal local
driver name") recline is refused, in the overlay and in `reclinectl`, while reset
still works. A streamed Frame keeps its identity (`Prop_ResourceRoot_String` and
`Prop_ControllerType_String` are `frame_hmd`, SteamVR's resource-only Frame
driver; model "Deckard MP"), and the message then says to install Recline VR on
the Frame, which covers streamed games. Other wireless streaming (Virtual Desktop,
ALVR) re-projects in its own way and is untested.

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

The rest is generic. A Linux x86-64 build has run in SteamVR with the null
headset driver (under WSL, where SteamVR's compositor can't start, so without the
overlay): the driver loads and hooks, and `reclinectl` sets, applies and resets a
recline. It hasn't been tested with a PC headset. A Windows build (MSVC, static CRT) passes the unit
tests and has run in SteamVR with the null headset driver: the driver loads and
hooks, the overlay auto-launches and `reclinectl` reaches the driver, but it
hasn't been tested with a real headset. Its shared memory is a
session-local named mapping (`Local\recline_v1`), which disappears with its last
user.

### Install layout

The install directory is the driver's root, with every binary in
`bin/<platform>/`:

    bin/linuxarm64/   driver_recline.so, librecline_xrshim.so, recline_overlay, reclinectl
    bin/linux64/      driver_recline.so, recline_overlay, reclinectl
    bin/win64/        driver_recline.dll, recline_overlay.exe, reclinectl.exe
    resources/        icon.png, settings/default.vrsettings, overlay/ (actions, bindings)
    driver.vrdrivermanifest, recline_overlay.vrmanifest, VERSION, install and uninstall scripts,
    LICENSE, THIRD_PARTY.md

SteamVR fixes the driver's part: it loads `bin/<platform>/driver_<name>` and reads
`resources/settings/default.vrsettings` under the directory registered with
`vrpathreg`. Putting the overlay and `reclinectl` next to the driver lets one
directory hold every platform, and the overlay manifest name each binary with its
platform's key (`binary_path_linux_arm`, `binary_path_linux`,
`binary_path_windows`), so it is a static file. The overlay and `reclinectl`
find the manifest and `resources/` two levels above their own executable, and
pass those paths to SteamVR with the platform's own separators: SteamVR on
Windows takes a manifest's directory from the last `\` only, so
`...\recline-vr/recline_overlay.vrmanifest` gives the app the wrong working
directory and SteamVR skips it. It also keeps a manifest path as first
registered, so `reclinectl unregister` removes the path even when SteamVR has
skipped the app. The overlay's action manifest and bindings
sit in `resources/overlay/` rather than `resources/input/`, which SteamVR uses for
drivers' own input profiles.

A build fills one `bin/<platform>/`; a release package has all three. The
installers (`install.sh` on Linux, `install.cmd` wrapping `install.ps1` on
Windows) copy only their own platform's binaries. Up to v1.2 the driver's root was
a `recline/` subdirectory, with the overlay beside it; `install.sh` unregisters
and removes that layout. On Windows a running SteamVR holds the driver DLL and the
overlay open, and files in use can be renamed but not overwritten, so
`install.ps1` moves a busy file aside and a later install removes it. Both
installers find `vrpathreg` themselves: on Linux in `/opt/steamvr` or a Steam
install (or `$VRPATHREG`), on Windows through the runtime path in
`openvrpaths.vrpath`, as the OpenVR loader does.

Registering the overlay needs a vrserver, but not a running SteamVR: when none
is running, a `VRApplication_Utility` client gets a short-lived vrserver that
loads no drivers and exits two seconds after the client leaves. So the Windows
installers register and unregister whether or not SteamVR is running, and
installing with it stopped is best: nothing is locked, and the driver and the
overlay start with SteamVR's next start without a restart. Not so on desktop
Linux: Steam runs SteamVR inside its runtime container, and a vrserver started by
a client outside it fails to load (its libraries live in the container). On the
Frame it works, but the short-lived vrserver brings up the whole stack (camera
driver, XRService, compositor, eye tracking) for each call. So the Linux
installers register only while SteamVR runs.

Registration lives in `reclinectl register` / `unregister` rather than in the
overlay, because on Windows the overlay is a GUI-subsystem program: SteamVR
starting a console program opens a console window. A GUI program has no output
for an installer to show, and PowerShell doesn't wait for it, so the console
program does the registering. The overlay logs to `overlay.log` in the user data
directory instead (see Dashboard panel).

### Release builds

`.github/workflows/build.yml` builds each platform, checks it, and merges the
three into `recline-vr-<version>.zip` (and the debug info into
`recline-vr-<version>-debug.zip`); a `v*` tag drafts a GitHub release with both.

- The Linux builds run in Valve's Steam Runtime SDK containers: sniper (glibc
  2.31, what Steam itself targets) for `linux64`, steamrt4 for `linuxarm64`.
  steamrt4's glibc (2.41) is newer than the Frame's (2.39), so the arm64 binaries
  are held to 2.39: `tools/check_binaries.sh` fails the build if a binary needs a
  newer glibc than its platform's limit, or any shared library besides glibc's. libstdc++ and libgcc are linked statically, and Vulkan and the OpenVR
  runtime are loaded at run time.
- The overlay needs only the Vulkan headers, so it looks for `vulkan/vulkan.h`
  itself rather than through FindVulkan, which also wants the loader library. CI
  sets `RECLINE_REQUIRE_VULKAN`, so a missing header fails the build instead of
  shipping the flickering `SetOverlayRaw` fallback.
- `tools/ci_linux.sh` is the whole Linux job, so it runs the same way in a local
  container (see its header).
- Everything outside `bin/` comes from the same commit, so
  `tools/merge_packages.sh` requires it to be identical across the three builds;
  this also catches line-ending conversion on the Windows checkout.
- GitHub's artifact archives drop Unix execute bits, so builds pass tarballs
  between jobs and the package is a zip made with Info-ZIP, which keeps them.

## Known limitations

- **Passthrough** is corrected by the XRService shim (verified in both the 3D and
  2D room-view modes). If the shim isn't loaded or one of its checks fails,
  passthrough is misaligned while reclined.
- **Boundaries** are drawn in the un-rotated frame and look tilted while reclined.
  Correcting them was ruled out. The compositor draws the walls as vertical
  extrusions of the floor outline in standing space, with no transform to tilt
  them. Committing rotated walls creates a new universe detached from the SLAM
  map. Replacing the chaperone would leave the user without one if Recline
  failed. Instead R is reset as soon as the user moves away or takes the headset
  off (see Automatic reset), so the boundary is right whenever they can walk.
  While lying still, hands are drawn displaced from their real positions, so the
  boundary may show when a virtual hand crosses it, or miss a real hand
  reaching a real wall beside the bed.
- **SteamVR safe mode** disables add-on drivers after a crash; the overlay reports
  the driver as not loaded.
