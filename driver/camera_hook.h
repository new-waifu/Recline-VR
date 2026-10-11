// Recline correction for passthrough camera frames.
//
// The Frame's passthrough camera frames reach vrserver through the HMD
// driver's camera component (IVRCameraComponent_003, implemented by driver_cv):
// vrserver calls GetVideoStreamFrame() and each frame carries its own
// DriverPose_t, which vrserver turns into the camera pose the compositor uses
// to project the image. That pose never passes through
// TrackedDevicePoseUpdated, so without this hook the image is projected from a
// pose that lacks R while everything else (head, room-view mesh) includes it.
//
// We capture device drivers as they are added (IVRServerDriverHost slot
// TrackedDeviceAdded), fetch the HMD's camera component once it is active, and
// patch its GetVideoStreamFrame vtable slot so each returned frame's
// world-from-driver gets R applied, exactly like ApplyWorldTransform does for
// device poses. Fails safe: any validation failure leaves frames untouched.
#pragma once

#include <functional>
#include <string>

#include <openvr_driver.h>

#include "recline_math.h"

namespace recline {

// Reads the current R; returns false if R is identity or unavailable. Must be
// lock-free (called from vrserver's camera thread).
using ReadRFn = bool (*)(Rigid*);

// Hooks IVRServerDriverHost::TrackedDeviceAdded on the host vtable to capture
// HMD device drivers. Call once from Init, before other drivers add devices.
bool InstallDeviceAddedHook(vr::IVRServerDriverHost* host, std::string* why);

// Call periodically (command thread). Once a captured HMD driver is active
// (`hmdActive`), fetches its camera component and hooks GetVideoStreamFrame.
// Logs progress through `log`; does nothing further once done or given up.
void MaybeHookCamera(bool hmdActive, ReadRFn readR,
                     const std::function<void(const std::string&)>& log);

// Pass-through from now on (driver Cleanup). Hooks stay installed if they
// can't be removed safely.
void DisableCameraHooks();

}  // namespace recline
