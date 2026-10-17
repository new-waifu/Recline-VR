// Layer hooks by vtable slot patch, validated against /proc/self/maps (as
// pose_hook_linux.cpp). The patches stay for the life of the process: the
// SubmitLayer one is in another driver's vtable, which may be unloaded before us.
#include "layer_hook.h"

#include "slot_patch.h"

namespace recline {

namespace {

void** g_submitSlot = nullptr;  // the SubmitLayer slot we patched, if any

// Points slot `idx` of `obj`'s vtable at `hook`; `*prev` receives what it held.
template <class Fn>
bool PatchVtableSlot(const void* obj, size_t idx, Fn hook, std::atomic<Fn>* prev, void*** slotOut,
                     std::string* why, std::string* info) {
  void** slot;
  void* cur;
  if (idx == SIZE_MAX) {
    *why = "could not derive the vtable index";
    return false;
  }
  if (!LocateVtableSlot(obj, idx, nullptr, &slot, &cur, why, info)) return false;
  if (cur == reinterpret_cast<void*>(hook)) {
    *why = "hook already installed (re-init)";
    return false;
  }
  // Store prev before the hook can run; chain to whatever was there.
  prev->store(reinterpret_cast<Fn>(cur), std::memory_order_release);
  std::string err;
  if (!PatchSlot(slot, cur, reinterpret_cast<void*>(hook), &err)) {
    *why = "patching vtable slot failed: " + err;
    return false;
  }
  if (!err.empty()) *info += "; " + err;
  *slotOut = slot;
  return true;
}

}  // namespace

bool InstallDeviceAddedHook(vr::IVRServerDriverHost* host, DeviceAddedFn hook,
                            std::atomic<DeviceAddedFn>* prev, std::string* why, std::string* info) {
  void** slot;
  return PatchVtableSlot(host, VirtualSlotIndex(&vr::IVRServerDriverHost::TrackedDeviceAdded), hook,
                         prev, &slot, why, info);
}

bool InstallSubmitLayerHook(vr::IVRDriverDirectModeComponent* comp, SubmitLayerFn hook,
                            std::atomic<SubmitLayerFn>* prev, std::string* module, std::string* why,
                            std::string* info) {
  const size_t idx = VirtualSlotIndex(&vr::IVRDriverDirectModeComponent::SubmitLayer);
  if (g_submitSlot) {
    // One SubmitLayer hook per process; the same driver re-adding its HMD
    // brings the same vtable.
    void** vtbl = *reinterpret_cast<void** const*>(comp);
    if (idx != SIZE_MAX && &vtbl[idx] == g_submitSlot) return true;
    *why = "another direct-mode driver's SubmitLayer is already hooked";
    return false;
  }
  void** slot;
  if (!PatchVtableSlot(comp, idx, hook, prev, &slot, why, info)) return false;
  g_submitSlot = slot;
  Mapping m;
  if (FindMapping(reinterpret_cast<uintptr_t>(prev->load()), &m)) *module = m.path;
  return true;
}

}  // namespace recline
