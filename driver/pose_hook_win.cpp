// Pose hook by MinHook detour of the function the vtable slot points to.
//
// Space Calibrator hooks the same function this way on Windows. A vtable patch
// would sit in front of every such detour regardless of install order, so the
// order would be wrong whenever it loads after us; detouring the function too
// makes a later hook wrap ours, as on Linux.
#include "pose_hook.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include <windows.h>

#include <MinHook.h>

#include "platform.h"

namespace recline {

namespace {

// --- vtable index ---------------------------------------------------------------
// MSVC represents a pointer to a virtual member function as the address of a
// vcall thunk that jumps through the object's vtable, so the index is not in the
// pointer. Calling it on a fake object whose vtable entries record their own
// position yields the index.
constexpr size_t kMaxSlots = 64;
size_t g_probed = SIZE_MAX;  // Init runs once, single-threaded

template <size_t I>
void Probe() {
  g_probed = I;
}
template <size_t... I>
constexpr std::array<void (*)(), sizeof...(I)> MakeProbes(std::index_sequence<I...>) {
  return {{&Probe<I>...}};
}

size_t PoseSlotIndex() {
  static constexpr auto probes = MakeProbes(std::make_index_sequence<kMaxSlots>{});
  struct FakeObject {
    const void* vtbl;
  } fake{probes.data()};
  // Called without arguments: the probes take none and the x64 caller owns the
  // stack, so the mismatch is harmless.
  using AnyFn = void (vr::IVRServerDriverHost::*)();
  const auto pmf = reinterpret_cast<AnyFn>(&vr::IVRServerDriverHost::TrackedDevicePoseUpdated);
  g_probed = SIZE_MAX;
  (reinterpret_cast<vr::IVRServerDriverHost*>(&fake)->*pmf)();
  return g_probed;
}

// --- validation -------------------------------------------------------------------
constexpr DWORD kWritable =
    PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr DWORD kExecutable =
    PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

bool Query(const void* p, MEMORY_BASIC_INFORMATION* m) {
  return VirtualQuery(p, m, sizeof *m) == sizeof *m && m->State == MEM_COMMIT;
}

// --- state ----------------------------------------------------------------------
void** g_slot = nullptr;
void* g_target = nullptr;
// The detour's jump, as written over the target's first bytes.
constexpr size_t kJumpBytes = 5;
uint8_t g_jump[kJumpBytes];

}  // namespace

bool InstallPoseHook(vr::IVRServerDriverHost* host, PoseFn hook, std::atomic<PoseFn>* prev,
                     std::string* why, std::string* info) {
  const size_t idx = PoseSlotIndex();
  if (idx >= kMaxSlots) {
    *why = "could not derive the vtable index";
    return false;
  }
  void** vtbl = *reinterpret_cast<void** const*>(host);
  void** slot = &vtbl[idx];
  MEMORY_BASIC_INFORMATION ms{}, mt{};
  if (!Query(slot, &ms)) {
    *why = "vtable slot is not mapped";
    return false;
  }
  void* target = std::atomic_ref<void*>(*slot).load(std::memory_order_acquire);
  if (!Query(target, &mt)) {
    *why = "slot target is not mapped";
    return false;
  }
  char buf[1024];
  snprintf(buf, sizeof buf, "obj=%p vtbl=%p index=%zu slot in %s (prot 0x%lx); target %p in %s",
           static_cast<void*>(host), static_cast<void*>(vtbl), idx, ModulePathOf(slot).c_str(),
           ms.Protect, target, ModulePathOf(target).c_str());
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
  void* trampoline = nullptr;
  MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(hook), &trampoline);
  if (st == MH_ERROR_ALREADY_CREATED) {
    *why = "hook already installed (re-init)";
    return false;
  }
  if (st != MH_OK) {
    *why = std::string("MH_CreateHook: ") + MH_StatusToString(st);
    return false;
  }
  // Store prev before the hook can run: the trampoline runs the original
  // function (or a hook installed before ours).
  prev->store(reinterpret_cast<PoseFn>(trampoline), std::memory_order_release);
  st = MH_EnableHook(target);
  if (st != MH_OK) {
    MH_RemoveHook(target);
    *why = std::string("MH_EnableHook: ") + MH_StatusToString(st);
    return false;
  }
  memcpy(g_jump, target, kJumpBytes);
  g_slot = slot;
  g_target = target;
  return true;
}

bool PoseHookWrapped() {
  if (!g_target) return false;
  // A later vtable patch, or a later detour rewriting our jump.
  return std::atomic_ref<void*>(*g_slot).load(std::memory_order_relaxed) != g_target ||
         memcmp(g_target, g_jump, kJumpBytes) != 0;
}

bool RemovePoseHook(std::string* msg) {
  if (!g_target) return false;
  if (PoseHookWrapped()) {
    // Disabling would restore the original bytes under the wrapper's detour.
    *msg = "hook was wrapped; staying installed as pass-through";
    return false;
  }
  const MH_STATUS st = MH_DisableHook(g_target);
  if (st != MH_OK) {
    *msg = std::string("could not remove hook (") + MH_StatusToString(st) +
           "); staying as pass-through";
    return false;
  }
  *msg = "hook removed";
  g_target = nullptr;
  return true;
}

}  // namespace recline
