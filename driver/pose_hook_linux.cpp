// Pose hook by vtable slot patch, validated against /proc/self/maps.
#include "pose_hook.h"

#include <cstdio>

#include "slot_patch.h"

namespace recline {

namespace {
void** g_slot = nullptr;  // &vtbl[index of TrackedDevicePoseUpdated]
void* g_hook = nullptr;
std::atomic<PoseFn>* g_prev = nullptr;
}  // namespace

bool InstallPoseHook(vr::IVRServerDriverHost* host, PoseFn hook, std::atomic<PoseFn>* prev,
                     std::string* why, std::string* info) {
  void** slot;
  void* cur;
  const size_t idx = VirtualSlotIndex(&vr::IVRServerDriverHost::TrackedDevicePoseUpdated);
  if (!LocateVtableSlot(host, idx, nullptr, &slot, &cur, why, info)) return false;
  if (cur == reinterpret_cast<void*>(hook)) {
    *why = "hook already installed (re-init)";
    return false;
  }
  // Store prev before the hook can run; chain to whatever was there (another
  // vtable hook or the original thunk).
  prev->store(reinterpret_cast<PoseFn>(cur), std::memory_order_release);
  std::string err;
  if (!PatchSlot(slot, cur, reinterpret_cast<void*>(hook), &err)) {
    *why = "patching vtable slot failed: " + err;
    return false;
  }
  if (!err.empty()) *info += "; " + err;
  g_slot = slot;
  g_hook = reinterpret_cast<void*>(hook);
  g_prev = prev;
  return true;
}

bool PoseHookWrapped() {
  return g_slot && std::atomic_ref<void*>(*g_slot).load(std::memory_order_relaxed) != g_hook;
}

bool RemovePoseHook(std::string* msg) {
  if (!g_slot) return false;
  void* cur = std::atomic_ref<void*>(*g_slot).load(std::memory_order_acquire);
  if (cur != g_hook) {
    char buf[96];
    snprintf(buf, sizeof buf, "hook was wrapped by %p; staying installed as pass-through", cur);
    *msg = buf;
    return false;
  }
  std::string err;
  if (!PatchSlot(g_slot, cur, reinterpret_cast<void*>(g_prev->load()), &err)) {
    *msg = "could not remove hook (" + err + "); staying as pass-through";
    return false;
  }
  *msg = "hook removed" + (err.empty() ? std::string() : "; " + err);
  g_slot = nullptr;
  return true;
}

}  // namespace recline
