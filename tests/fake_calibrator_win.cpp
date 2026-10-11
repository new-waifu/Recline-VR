// Space Calibrator on Windows: a MinHook detour of the function in vtable slot 1,
// from its own module with its own copy of MinHook. Built as a DLL.
#include "fake_calibrator.h"

#include <MinHook.h>

bool InstallFakeCalibrator(vr::IVRServerDriverHost* host, recline::PoseFn hook,
                           recline::PoseFn* prev, std::string* why) {
  void* target = (*reinterpret_cast<void** const*>(host))[1];
  MH_STATUS st = MH_Initialize();
  if (st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED)
    st = MH_CreateHook(target, reinterpret_cast<void*>(hook), reinterpret_cast<void**>(prev));
  if (st == MH_OK) st = MH_EnableHook(target);
  if (st != MH_OK) *why = MH_StatusToString(st);
  return st == MH_OK;
}
