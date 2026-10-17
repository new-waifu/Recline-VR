// Layer hooks by MinHook detour of the functions the vtable slots point to (as
// pose_hook_win.cpp does). The detours stay installed for the life of the
// process: the SubmitLayer one patches another driver's code, which may be
// unloaded before us.
#include "layer_hook.h"

#include <cstdint>
#include <cstdio>

#include <windows.h>

#include <MinHook.h>

#include "platform.h"
#include "vtable_index_win.h"

namespace recline {

namespace {

constexpr DWORD kWritable =
    PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr DWORD kExecutable =
    PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

bool Query(const void* p, MEMORY_BASIC_INFORMATION* m) {
  return VirtualQuery(p, m, sizeof *m) == sizeof *m && m->State == MEM_COMMIT;
}

void* g_submitTarget = nullptr;  // the SubmitLayer we detoured, if any

// Detours the function in slot `idx` of `obj`'s vtable, after checking the slot
// is in a read-only image and its target in an executable one. Sets `*target`.
bool Detour(const void* obj, size_t idx, void* hook, void** trampoline, void** target,
            std::string* why, std::string* info) {
  if (idx == SIZE_MAX) {
    *why = "could not derive the vtable index";
    return false;
  }
  void* const* vtbl = *reinterpret_cast<void* const* const*>(obj);
  void* const* slot = &vtbl[idx];
  MEMORY_BASIC_INFORMATION ms{}, mt{};
  if (!Query(slot, &ms)) {
    *why = "vtable slot is not mapped";
    return false;
  }
  void* t = *slot;
  if (!Query(t, &mt)) {
    *why = "slot target is not mapped";
    return false;
  }
  char buf[1024];
  snprintf(buf, sizeof buf, "obj=%p index=%zu slot in %s; target %p in %s", obj, idx,
           ModulePathOf(slot).c_str(), t, ModulePathOf(t).c_str());
  *info = buf;
  if (ms.Type != MEM_IMAGE || (ms.Protect & kWritable)) {
    *why = "vtable slot is not in a readonly image mapping";
    return false;
  }
  if (mt.Type != MEM_IMAGE || !(mt.Protect & kExecutable)) {
    *why = "slot target is not in an executable image mapping";
    return false;
  }
  const MH_STATUS init = MH_Initialize();
  if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
    *why = std::string("MH_Initialize: ") + MH_StatusToString(init);
    return false;
  }
  MH_STATUS st = MH_CreateHook(t, hook, trampoline);
  if (st != MH_OK) {
    *why = std::string("MH_CreateHook: ") + MH_StatusToString(st);
    return false;
  }
  *target = t;
  return true;
}

bool Enable(void* target, std::string* why) {
  const MH_STATUS st = MH_EnableHook(target);
  if (st == MH_OK) return true;
  MH_RemoveHook(target);
  *why = std::string("MH_EnableHook: ") + MH_StatusToString(st);
  return false;
}

}  // namespace

bool InstallDeviceAddedHook(vr::IVRServerDriverHost* host, DeviceAddedFn hook,
                            std::atomic<DeviceAddedFn>* prev, std::string* why, std::string* info) {
  void* trampoline = nullptr;
  void* target = nullptr;
  if (!Detour(host, VtableIndex(&vr::IVRServerDriverHost::TrackedDeviceAdded),
              reinterpret_cast<void*>(hook), &trampoline, &target, why, info))
    return false;
  prev->store(reinterpret_cast<DeviceAddedFn>(trampoline), std::memory_order_release);
  return Enable(target, why);
}

bool InstallSubmitLayerHook(vr::IVRDriverDirectModeComponent* comp, SubmitLayerFn hook,
                            std::atomic<SubmitLayerFn>* prev, std::string* module, std::string* why,
                            std::string* info) {
  const size_t idx = VtableIndex(&vr::IVRDriverDirectModeComponent::SubmitLayer);
  if (g_submitTarget) {
    // One SubmitLayer hook per process; the same driver re-adding its HMD
    // brings the same function.
    void* const* vtbl = *reinterpret_cast<void* const* const*>(comp);
    if (idx != SIZE_MAX && vtbl[idx] == g_submitTarget) return true;
    *why = "another direct-mode driver's SubmitLayer is already hooked";
    return false;
  }
  void* trampoline = nullptr;
  void* target = nullptr;
  if (!Detour(comp, idx, reinterpret_cast<void*>(hook), &trampoline, &target, why, info))
    return false;
  prev->store(reinterpret_cast<SubmitLayerFn>(trampoline), std::memory_order_release);
  if (!Enable(target, why)) return false;
  g_submitTarget = target;
  *module = ModulePathOf(target);
  return true;
}

}  // namespace recline
