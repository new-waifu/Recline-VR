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
};

// pose_count is sampled twice, `aliveWindowSec` apart, unless a previous
// sample is supplied (non-blocking UI use).
DriverStatus QueryStatus(ReclineShm* shm, uint64_t* prevPoseCount = nullptr,
                         double aliveWindowSec = 0.1);

enum class SendResult { Ok, NoHmdPose, InvalidArgs, Timeout, LockTimeout, Other };

const char* SendResultText(SendResult r);

// Write a command and wait for the driver's ack.
SendResult SendCommand(ReclineShm* shm, uint32_t cmd, const Rigid* payload,
                       StateSnapshot* stateOut = nullptr, double timeoutSec = 0.5);

}  // namespace recline
