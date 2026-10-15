// Client-side helpers shared by recline_overlay and reclinectl:
// building the recenter target and the command handshake (write a command,
// then wait for the driver to acknowledge its id).
#pragma once

#include <openvr.h>

#include <cstdint>
#include <string>

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
  Trust trust = Trust::None;
  Level level = Bad;
  std::string line;   // status line (panel, reclinectl status)
  std::string toast;  // short form, for a toast; empty when there is nothing to do
};

// The driver's state compared with this client's own build and install.
// `shm` is the open segment, or null with `openError` and `refused` (what
// OpenShm found) explaining why. `settings` (may be null) tells whether
// SteamVR disabled the driver.
Diagnosis Diagnose(const ReclineShm* shm, const std::string& openError, const DriverIdentity& refused,
                   const DriverStatus& st, vr::IVRSettings* settings);

enum class SendResult { Ok, NoHmdPose, InvalidArgs, Timeout, LockTimeout, Other };

const char* SendResultText(SendResult r);

// Write a command and wait for the driver's ack.
SendResult SendCommand(ReclineShm* shm, uint32_t cmd, const Rigid* payload,
                       StateSnapshot* stateOut = nullptr, double timeoutSec = 0.5);

// The overlay's SteamVR app key (packaging/recline_overlay.vrmanifest).
constexpr const char* kOverlayAppKey = "user.recline.overlay";

// `rel` (written with '/') under the install root, which holds the manifests
// and resources/; the running binary sits two levels below it, in
// bin/<platform>/, where SteamVR requires the driver. The result has the
// platform's own separators: SteamVR on Windows splits paths at '\' only, so a
// mixed path gives the overlay manifest the wrong directory.
std::string InstallFile(const char* rel);

}  // namespace recline
