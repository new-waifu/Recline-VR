// Client-side helpers shared by recline_overlay and reclinectl:
// building the recenter target and the command handshake (write a command,
// then wait for the driver to acknowledge its id).
#pragma once

#include <openvr.h>

#include <cstdint>
#include <string>
#include <vector>

#include "recenter_math.h"
#include "recline_math.h"
#include "recline_shm.h"

namespace recline {

// HMD pose in the given universe (no prediction). False if not tracking.
bool GetHmdPose(vr::IVRSystem* sys, vr::ETrackingUniverseOrigin uni, Rigid* out);

// View pitch of the lens centre relative to the head axis, averaged over both
// eyes (see ViewPitchFromProjection). Returns 0 if nothing usable is reported.
double MeasureViewPitch(vr::IVRSystem* sys);

// Target head pose in raw space for a recenter, from the current HMD poses.
bool BuildTraw(vr::IVRSystem* sys, const RecenterOptions& o, Rigid* out, std::string* err);

struct DriverStatus {
  bool shmOk = false;
  bool driverAlive = false;  // pose_count advancing
  StateSnapshot state;
  HmdSnapshot hmd;
  DriverIdentity identity;
};

// pose_count is sampled twice, `aliveWindowSec` apart, unless a previous
// sample is supplied (non-blocking UI use).
DriverStatus QueryStatus(ReclineShm* shm, uint64_t* prevPoseCount = nullptr,
                         double aliveWindowSec = 0.1);

// How far a client may act on what the driver publishes (docs/DESIGN.md,
// "Acting on the driver's state").
enum class Trust {
  None,     // no driver, or not this build's: its state is for display only
  Cleanup,  // this build's driver, but newer files are installed: undo only
  Full,
};

struct Diagnosis {
  enum Level { Good, Info, Warn, Bad };
  enum Cause {
    Ok, AlsoRegistered, HookRefused, NoPoses, Updated, OtherInstall, OtherVersion, Unreadable,
    SafeMode, Disabled, NotRegistered, NotLoaded,
  };
  Cause cause = NotLoaded;
  Trust trust = Trust::None;
  Level level = Bad;
  std::string line;   // status line (panel, reclinectl status)
  std::string toast;  // short form, for a toast; empty when there is nothing to do
};

// The driver's state compared with this client's own build and install.
// `shm` is the open segment, or null with `refused` (what OpenShm found, if
// anything). `settings` (may be null) tells whether SteamVR disabled the driver.
Diagnosis Diagnose(const ReclineShm* shm, const DriverIdentity& refused, const DriverStatus& st,
                   vr::IVRSettings* settings);

// Which Recline installs SteamVR loads drivers from (`external_drivers` in
// openvrpaths.vrpath, found as the OpenVR loader finds it). Read at most every
// 2 s; the overlay asks 4 times a second.
struct DriverRegistration {
  bool readable = false;
  bool ours = false;  // this client's install is among them
  std::vector<std::string> reclineRoots;
};
DriverRegistration ReadDriverRegistration();

// How the headset reaches this SteamVR. Over Steam Link (SteamVR's vrlink
// driver) the headset re-projects every frame to its own head pose, which
// undoes a recline made here unless the driver corrects the frames' render
// pose (FLAG_LAYERS_CORRECTED); without that, recline is refused. A streamed
// Steam Frame is always refused: it draws the dashboard itself, un-reclined,
// and Recline VR installed on the Frame covers streamed games anyway
// (docs/DESIGN.md, "Streaming to a PC"). A Steam Frame keeps its identity when
// streamed.
struct Headset {
  bool steamLink = false;  // carried by vrlink
  bool frame = false;      // a Steam Frame (frame_hmd)
};
Headset QueryHeadset(vr::IVRSystem* sys);
// Recline is refused: a streamed Steam Frame, or Steam Link and the driver
// doesn't correct its frames (`flags`: StateBlock::flags).
bool SteamLinkBlocked(const Headset& h, uint32_t flags);
// Why recline is refused over Steam Link: a status line and a toast.
std::string SteamLinkLine(const Headset& h);
constexpr const char* kSteamLinkToast = "Recline VR doesn't work over Steam Link";

enum class SendResult { Ok, NoHmdPose, InvalidArgs, Timeout, LockTimeout, Other };

const char* SendResultText(SendResult r);

// Write a command and wait for the driver's ack.
SendResult SendCommand(ReclineShm* shm, uint32_t cmd, const Rigid* payload,
                       StateSnapshot* stateOut = nullptr, double timeoutSec = 0.5);

// The overlay's SteamVR app key (packaging/recline_overlay.vrmanifest).
constexpr const char* kOverlayAppKey = "user.recline.overlay";

// Opens SteamVR's binding editor on the overlay's bindings (`set` may be
// k_ulInvalidActionSetHandle), on the page of a connected hand's controller.
bool OpenOverlayBindingUI(vr::VRActionSetHandle_t set, vr::EVRInputError* err);

// `rel` (written with '/') under the install root, which holds the manifests
// and resources/; the running binary sits two levels below it, in
// bin/<platform>/, where SteamVR requires the driver. The result has the
// platform's own separators: SteamVR on Windows splits paths at '\' only, so a
// mixed path gives the overlay manifest the wrong directory.
std::string InstallFile(const char* rel);

}  // namespace recline
