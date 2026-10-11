// Space Calibrator on Linux: a vtable slot patch, installed after ours.
#include "fake_calibrator.h"

#include "slot_patch.h"

bool InstallFakeCalibrator(vr::IVRServerDriverHost* host, recline::PoseFn hook,
                           recline::PoseFn* prev, std::string* why) {
  using namespace recline;
  void** slot;
  void* cur;
  std::string info;
  const size_t idx = VirtualSlotIndex(&vr::IVRServerDriverHost::TrackedDevicePoseUpdated);
  if (!LocateVtableSlot(host, idx, nullptr, &slot, &cur, why, &info)) return false;
  *prev = reinterpret_cast<PoseFn>(cur);
  std::string err;
  if (!PatchSlot(slot, cur, reinterpret_cast<void*>(hook), &err)) {
    *why = err;
    return false;
  }
  return true;
}
