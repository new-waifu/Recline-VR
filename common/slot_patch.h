// Helpers for patching code pointers in place: vtable slots (the driver's
// IVRServerDriverHost hooks, the XRService shim's IVRPaths hook) and GOT
// entries. Nothing here is build-specific: vtable indices are derived from the
// headers, and every address is validated against /proc/self/maps before
// writing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace recline {

// Vtable index of a virtual member function, from the Itanium C++ ABI
// representation of a pointer to member function. Generic Itanium: ptr = 1 +
// byte offset. ARM variant (aarch64): ptr = byte offset, virtual flag in bit 0
// of adj. SIZE_MAX if `pmf` is not virtual.
template <class PMF>
size_t VirtualSlotIndex(PMF pmf) {
  struct {
    uintptr_t ptr;
    ptrdiff_t adj;
  } rep;
  static_assert(sizeof(pmf) == sizeof(rep), "unexpected member pointer size");
  memcpy(&rep, &pmf, sizeof rep);
#if defined(__aarch64__) || defined(__arm__)
  if (!(rep.adj & 1)) return SIZE_MAX;
  return rep.ptr / sizeof(void*);
#else
  if (!(rep.ptr & 1)) return SIZE_MAX;
  return (rep.ptr - 1) / sizeof(void*);
#endif
}

struct Mapping {
  uintptr_t start = 0, end = 0;
  char perms[5] = {};
  std::string path;
  int Prot() const;
};

// The /proc/self/maps entry containing `addr`.
bool FindMapping(uintptr_t addr, Mapping* out);

// Validates slot `index` of `obj`'s vtable and its current target against
// /proc/self/maps: the slot must be in a read-only file-backed mapping and the
// target in an executable one. With `module` set, both mappings must also
// belong to a file whose path ends in "/<module>". On success returns the slot
// pointer and current target; does not patch. Once both addresses are found
// mapped, `info` receives a description (addresses, mappings) for logging,
// also when the mappings are then rejected.
bool LocateVtableSlot(const void* obj, size_t index, const char* module, void*** slotOut,
                      void** curOut, std::string* why, std::string* info);

// Atomically replace *slot (expected -> desired), making the page writable
// first if needed and restoring its protection after. On success `err` may
// still hold a warning (protection could not be restored).
bool PatchSlot(void** slot, void* expected, void* desired, std::string* err);

// True if `path` ends in "/<name>" (or equals `name`).
bool PathHasBasename(const char* path, const char* name);

// Locates the PLT/GOT entry for an imported symbol in the currently loaded
// module whose path ends in "/<module>". Walks the loaded images with
// dl_iterate_phdr and reads the module's .rela.plt in memory, so no offset is
// hardcoded. On success `*slotOut` is the writable GOT slot and `*curOut` its
// current value (the resolved function or the PLT resolver trampoline).
bool FindPltGotSlot(const char* module, const char* symbol, void*** slotOut, void** curOut,
                    std::string* why);

// True if a loaded image's path ends in "/<module>".
bool ModuleLoaded(const char* module);

}  // namespace recline
