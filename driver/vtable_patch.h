// Helpers for patching the shared IVRServerDriverHost vtable slot of
// TrackedDevicePoseUpdated. Nothing here is build-specific: the
// slot index is derived from openvr_driver.h, and every address is validated
// against /proc/self/maps before writing.
#pragma once

#include <openvr_driver.h>

#include <string>

namespace recline {

// Validates the TrackedDevicePoseUpdated slot of `host`'s vtable and its
// current target against /proc/self/maps. On success returns the slot pointer
// and current target; does not patch. Once both addresses are found mapped,
// `info` receives a description (addresses, mappings) for logging, also when
// the mappings are then rejected.
bool LocateSlot(vr::IVRServerDriverHost* host, void*** slotOut, void** curOut, std::string* why,
                std::string* info);

// Atomically replace *slot (expected -> desired) in a read-only page, then
// restore the page's original protection. On success `err` may still hold a
// warning (protection could not be restored).
bool PatchSlot(void** slot, void* expected, void* desired, std::string* err);

}  // namespace recline
