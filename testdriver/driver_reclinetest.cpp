// driver_reclinetest: coexistence test driver for SPEC §10.6. It vtable-hooks
// TrackedDevicePoseUpdated exactly like driver_recline (chaining to whatever is
// there) and applies a fixed world yaw + X offset to every device, the way a
// Space Calibrator-style tool would. Install it with loadPriority above or
// below driver_recline's (100) to test both hook orders.
//
// Settings section "driver_reclinetest": loadPriority, yawDeg, offsetX.

#include <openvr_driver.h>

#include <atomic>
#include <cstdarg>
#include <cstring>
#include <string>

#include <dlfcn.h>

#include "pose_math.h"
#include "vtable_patch.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace {

using namespace recline;
using PoseFn = void (*)(vr::IVRServerDriverHost*, uint32_t, const vr::DriverPose_t&, uint32_t);

std::atomic<PoseFn> g_prev{nullptr};
std::atomic<bool> g_disabled{false};
void** g_slot = nullptr;
Rigid g_T;  // written once in Init before the hook is installed

void Log(const char* fmt, ...) {
  char buf[1024];
  int n = snprintf(buf, sizeof buf, "[reclinetest] ");
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + n, sizeof buf - n, fmt, ap);
  va_end(ap);
  if (vr::VRDriverLog()) vr::VRDriverLog()->Log(buf);
}

void Hook(vr::IVRServerDriverHost* self, uint32_t id, const vr::DriverPose_t& in, uint32_t size) {
  const PoseFn prev = g_prev.load(std::memory_order_acquire);
  if (size != sizeof(vr::DriverPose_t) || g_disabled.load(std::memory_order_relaxed))
    return prev(self, id, in, size);
  vr::DriverPose_t p = in;
  ApplyWorldTransform(g_T, &p);
  prev(self, id, p, size);
}

class Provider : public vr::IServerTrackedDeviceProvider {
 public:
  vr::EVRInitError Init(vr::IVRDriverContext* ctx) override {
    VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&Hook), &info) && info.dli_fname)
      dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE);
    const float yaw = vr::VRSettings()->GetFloat("driver_reclinetest", "yawDeg");
    const float ox = vr::VRSettings()->GetFloat("driver_reclinetest", "offsetX");
    g_T = {AxisAngle({0, 1, 0}, Rad(yaw)), {ox, 0, 0}};
    void** slot;
    void* cur;
    std::string why, infoStr, err;
    const bool located = LocateSlot(vr::VRServerDriverHost(), &slot, &cur, &why, &infoStr);
    Log("%s", infoStr.c_str());
    if (!located) {
      Log("not hooking: %s", why.c_str());
      return vr::VRInitError_None;
    }
    g_prev.store(reinterpret_cast<PoseFn>(cur), std::memory_order_release);
    if (!PatchSlot(slot, cur, reinterpret_cast<void*>(&Hook), &err)) {
      Log("patch failed: %s", err.c_str());
      return vr::VRInitError_None;
    }
    g_slot = slot;
    Log("hooked (yaw %.1f deg, offsetX %.2f m), chained to %p", yaw, ox, cur);
    return vr::VRInitError_None;
  }
  void Cleanup() override {
    g_disabled.store(true);
    if (g_slot) {
      void* cur = __atomic_load_n(g_slot, __ATOMIC_ACQUIRE);
      std::string err;
      if (cur == reinterpret_cast<void*>(&Hook) &&
          PatchSlot(g_slot, cur, reinterpret_cast<void*>(g_prev.load()), &err))
        Log("unhooked");
      else
        Log("wrapped by %p; staying as pass-through", cur);
    }
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
  }
  const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
  void RunFrame() override {}
  bool ShouldBlockStandbyMode() override { return false; }
  void EnterStandby() override {}
  void LeaveStandby() override {}
};

Provider g_provider;

}  // namespace

EXPORT void* HmdDriverFactory(const char* name, int* rc) {
  if (strcmp(name, vr::IServerTrackedDeviceProvider_Version) == 0) return &g_provider;
  if (rc) *rc = vr::VRInitError_Init_InterfaceNotFound;
  return nullptr;
}
