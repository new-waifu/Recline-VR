// The hook on IVRServerDriverHost::TrackedDevicePoseUpdated, with one
// implementation per platform: pose_hook_linux.cpp patches the vtable slot,
// pose_hook_win.cpp detours the target function with MinHook (as Space
// Calibrator does there, so the two chain). Either way a hook installed after
// ours runs before it, i.e. wraps it (docs/DESIGN.md, "Coexistence").
#pragma once

#include <openvr_driver.h>

#include <atomic>
#include <string>

namespace recline {

using PoseFn = void (*)(vr::IVRServerDriverHost*, uint32_t, const vr::DriverPose_t&, uint32_t);

// Installs `hook` in front of whatever currently handles the call. `*prev`
// receives the function to chain to before `hook` can first run. `info`
// describes the target for logging, when known (also on failure).
bool InstallPoseHook(vr::IVRServerDriverHost* host, PoseFn hook, std::atomic<PoseFn>* prev,
                     std::string* why, std::string* info);

// True once another hook has been installed in front of ours.
bool PoseHookWrapped();

// Removes the hook unless something wrapped it, in which case it must stay
// installed (as pass-through): the wrapper chains into it. `msg` says what
// happened.
bool RemovePoseHook(std::string* msg);

}  // namespace recline
