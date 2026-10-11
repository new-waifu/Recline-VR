// Helpers for patching the shared IVRServerDriverHost vtable slot of
// TrackedDevicePoseUpdated. Nothing here is build-specific: the
// slot index is derived from openvr_driver.h, and every address is validated
// against /proc/self/maps before writing.
#pragma once

#include <openvr_driver.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <sys/mman.h>
#include <unistd.h>

namespace recline {

// Vtable index of TrackedDevicePoseUpdated, derived from the header through
// the Itanium C++ ABI representation of a pointer to virtual member function.
// Generic Itanium: ptr = 1 + byte offset. ARM variant (aarch64): ptr = byte
// offset, virtual flag in bit 0 of adj.
inline size_t PoseSlotIndex() {
  auto pmf = &vr::IVRServerDriverHost::TrackedDevicePoseUpdated;
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
  int Prot() const {
    return (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) |
           (perms[2] == 'x' ? PROT_EXEC : 0);
  }
};

inline bool FindMapping(uintptr_t addr, Mapping* out) {
  FILE* f = fopen("/proc/self/maps", "re");
  if (!f) return false;
  char line[4096];
  bool found = false;
  while (fgets(line, sizeof line, f)) {
    unsigned long s, e;
    char perms[5];
    int pathOff = 0;
    if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %n", &s, &e, perms, &pathOff) < 3) continue;
    if (addr < s || addr >= e) continue;
    out->start = s;
    out->end = e;
    memcpy(out->perms, perms, 5);
    out->path = pathOff > 0 ? std::string(line + pathOff) : std::string();
    while (!out->path.empty() && (out->path.back() == '\n' || out->path.back() == ' '))
      out->path.pop_back();
    found = true;
    break;
  }
  fclose(f);
  return found;
}

// Atomically replace *slot (expected -> desired) in a read-only page.
inline bool PatchSlot(void** slot, void* expected, void* desired, std::string* err) {
  Mapping m;
  if (!FindMapping(uintptr_t(slot), &m)) {
    *err = "slot not mapped";
    return false;
  }
  const uintptr_t pg = uintptr_t(sysconf(_SC_PAGESIZE));
  const uintptr_t first = uintptr_t(slot) & ~(pg - 1);
  const uintptr_t last = (uintptr_t(slot) + sizeof(void*) - 1) & ~(pg - 1);
  if (last + pg > m.end) {  // can't happen for an aligned slot; be exact anyway
    *err = "slot straddles mappings";
    return false;
  }
  const size_t len = last + pg - first;
  const int origProt = m.Prot();
  if (!(origProt & PROT_WRITE) &&
      mprotect(reinterpret_cast<void*>(first), len, PROT_READ | PROT_WRITE) != 0) {
    *err = std::string("mprotect RW: ") + strerror(errno);
    return false;
  }
  void* exp = expected;
  const bool ok = __atomic_compare_exchange_n(slot, &exp, desired, false, __ATOMIC_RELEASE,
                                              __ATOMIC_RELAXED);
  if (!(origProt & PROT_WRITE) && mprotect(reinterpret_cast<void*>(first), len, origProt) != 0)
    *err = std::string("warning: failed to restore page protection ") + m.perms + ": " +
           strerror(errno);
  if (!ok) *err = "slot changed concurrently";
  return ok;
}

// Validates the slot and its current target. On success returns the slot
// pointer and current target; does not patch.
inline bool LocateSlot(vr::IVRServerDriverHost* host, void*** slotOut, void** curOut,
                       std::string* why, std::string* info) {
  const size_t idx = PoseSlotIndex();
  if (idx == SIZE_MAX || idx > 64) {
    *why = "could not derive vtable index of TrackedDevicePoseUpdated";
    return false;
  }
  void** vtbl = *reinterpret_cast<void***>(host);
  void** slot = &vtbl[idx];
  Mapping ms, mf;
  if (!FindMapping(uintptr_t(slot), &ms)) {
    *why = "vtable slot is not mapped";
    return false;
  }
  void* cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (!FindMapping(uintptr_t(cur), &mf)) {
    *why = "slot target is not mapped";
    return false;
  }
  char buf[1024];
  snprintf(buf, sizeof buf, "host=%p vtbl=%p index=%zu slot in %s %s; target %p in %s %s",
           static_cast<void*>(host), static_cast<void*>(vtbl), idx, ms.perms, ms.path.c_str(), cur,
           mf.perms, mf.path.c_str());
  *info = buf;
  if (ms.perms[0] != 'r' || ms.perms[2] == 'x' || ms.path.empty() || ms.path[0] != '/') {
    *why = "vtable slot is not in a readonly file-backed image mapping";
    return false;
  }
  if (mf.perms[0] != 'r' || mf.perms[2] != 'x' || mf.path.empty()) {
    *why = "slot target is not in an executable image mapping";
    return false;
  }
  *slotOut = slot;
  *curOut = cur;
  return true;
}

}  // namespace recline
