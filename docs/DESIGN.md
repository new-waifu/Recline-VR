# Recline Recenter: design notes

How and why Recline Recenter works the way it does. Observations about SteamVR
internals were made on the Steam Frame's SteamVR build of October 2026; the code
deliberately depends only on the public, versioned OpenVR driver ABI, never on
build-specific addresses or byte patterns.

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
  `/proc/self/maps`: the slot must sit in a read-only, file-backed image mapping and
  the target in an executable one. Otherwise the hook is refused and reported.
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
the user. The standing zero pose is never touched: it is persisted and synced to
connected PCs.

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

## Coexistence with other pose hooks

Other tools (e.g. Space Calibrator-style calibrators) may hook the same slot.

- A driver's `loadPriority` decides `Init` order (higher loads earlier). We use 100,
  so vtable hooks installed by drivers that load after us wrap ours: they run first,
  and our R is applied on top of their result, which is what we want.
- Hooks installed earlier, inline detours, or lazy hooks can still end up after us.
  That can't be prevented without build-specific hooks, so it is detected instead:
  - `conflict_wrapped`: about once a second, the driver re-reads the slot; if it no
    longer points at our hook, someone wrapped us.
  - `conflict_downstream`: about once a second, after forwarding an HMD pose, the
    hook compares `GetRawTrackedDevicePoses` with `R ∘ H`. A persistent mismatch
    means poses are modified after us. On the Frame this API returns post-transform
    poses; if a runtime instead returned driver-space poses, the driver detects that
    and disables the check rather than report false conflicts.
- Both flags are shown in the overlay. The driver never unhooks itself because of them.
- PC-side calibration (e.g. Space Calibrator on a streaming PC) is invalidated by a
  recenter; the overlay says so.

## Streaming to a PC

On the Frame, the PC-streaming client is an ordinary OpenVR app that reads poses
from the compositor and sends them to the PC, so a Frame-side transform should reach
streamed games with nothing installed on the PC. This is not yet verified.

## Known limitations

- **Passthrough** is wrong while reclined, in both 3D and 2D room-view modes. The
  camera images and room mesh are placed using poses that don't include R.
- **Boundaries** are drawn in the un-rotated frame and look tilted while reclined.
- **SteamVR safe mode** disables add-on drivers after a crash; the overlay reports
  the driver as not loaded.
