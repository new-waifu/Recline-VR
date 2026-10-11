// A stand-in for Space Calibrator's pose hook in test_pose_hook: hooks
// TrackedDevicePoseUpdated the way it does on each platform
// (fake_calibrator_linux.cpp: vtable slot patch; fake_calibrator_win.cpp: MinHook
// detour from a DLL with its own MinHook copy).
#pragma once

#include <string>

#include "pose_hook.h"

// `hook` must chain to `*prev`, which is set before the hook can first run.
bool InstallFakeCalibrator(vr::IVRServerDriverHost* host, recline::PoseFn hook,
                           recline::PoseFn* prev, std::string* why);
