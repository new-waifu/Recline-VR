#include "slot_patch.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

namespace recline {

int Mapping::Prot() const {
  return (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) |
         (perms[2] == 'x' ? PROT_EXEC : 0);
}

bool FindMapping(uintptr_t addr, Mapping* out) {
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

bool PathHasBasename(const char* path, const char* name) {
  if (!path || !name) return false;
  const size_t lp = strlen(path), ln = strlen(name);
  if (lp < ln || strcmp(path + lp - ln, name) != 0) return false;
  return lp == ln || path[lp - ln - 1] == '/';
}

namespace {

struct PltQuery {
  const char* module;
  const char* symbol;
  void** slot = nullptr;
  bool triedModule = false;
};

// One PT_DYNAMIC image: resolve a named PLT relocation to its GOT slot.
int PltCallback(dl_phdr_info* info, size_t, void* data) {
  auto* q = static_cast<PltQuery*>(data);
  if (!info->dlpi_name || !PathHasBasename(info->dlpi_name, q->module)) return 0;
  q->triedModule = true;

  const ElfW(Dyn)* dyn = nullptr;
  for (int i = 0; i < info->dlpi_phnum; ++i)
    if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
      dyn = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
  if (!dyn) return 1;

  const ElfW(Sym)* symtab = nullptr;
  const char* strtab = nullptr;
  const char* jmprel = nullptr;
  size_t pltrelsz = 0, relaent = sizeof(ElfW(Rela));
  for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
    switch (d->d_tag) {
      case DT_SYMTAB: symtab = reinterpret_cast<const ElfW(Sym)*>(d->d_un.d_ptr); break;
      case DT_STRTAB: strtab = reinterpret_cast<const char*>(d->d_un.d_ptr); break;
      case DT_JMPREL: jmprel = reinterpret_cast<const char*>(d->d_un.d_ptr); break;
      case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
      case DT_RELAENT: relaent = d->d_un.d_val; break;
      default: break;
    }
  }
  if (!symtab || !strtab || !jmprel || !pltrelsz) return 1;

  for (size_t off = 0; off < pltrelsz; off += relaent) {
    const auto* r = reinterpret_cast<const ElfW(Rela)*>(jmprel + off);
    const unsigned long symIdx = ELF64_R_SYM(r->r_info);
    const char* name = strtab + symtab[symIdx].st_name;
    if (strcmp(name, q->symbol) == 0) {
      q->slot = reinterpret_cast<void**>(info->dlpi_addr + r->r_offset);
      return 1;
    }
  }
  return 1;  // module found but symbol absent: stop, slot stays null
}

}  // namespace

namespace {
int ModuleCallback(dl_phdr_info* info, size_t, void* data) {
  const char* module = *static_cast<const char**>(data);
  if (info->dlpi_name && PathHasBasename(info->dlpi_name, module)) {
    *static_cast<const char**>(data) = nullptr;  // signal found
    return 1;
  }
  return 0;
}
}  // namespace

bool ModuleLoaded(const char* module) {
  const char* probe = module;
  dl_iterate_phdr(ModuleCallback, &probe);
  return probe == nullptr;
}

bool FindPltGotSlot(const char* module, const char* symbol, void*** slotOut, void** curOut,
                    std::string* why) {
  PltQuery q{module, symbol};
  dl_iterate_phdr(PltCallback, &q);
  if (!q.triedModule) {
    *why = std::string(module) + " is not loaded";
    return false;
  }
  if (!q.slot) {
    *why = std::string(symbol) + " has no PLT relocation in " + module;
    return false;
  }
  Mapping m;
  if (!FindMapping(uintptr_t(q.slot), &m)) {
    *why = "GOT slot is not mapped";
    return false;
  }
  *slotOut = q.slot;
  *curOut = __atomic_load_n(q.slot, __ATOMIC_ACQUIRE);
  return true;
}

bool PatchSlot(void** slot, void* expected, void* desired, std::string* err) {
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
      mprotect(reinterpret_cast<void*>(first), len, origProt | PROT_WRITE) != 0) {
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

bool LocateVtableSlot(const void* obj, size_t index, const char* module, void*** slotOut,
                      void** curOut, std::string* why, std::string* info) {
  if (index == SIZE_MAX || index > 64) {
    *why = "could not derive the vtable index";
    return false;
  }
  void** vtbl = *reinterpret_cast<void** const*>(obj);
  void** slot = &vtbl[index];
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
  snprintf(buf, sizeof buf, "obj=%p vtbl=%p index=%zu slot in %s %s; target %p in %s %s", obj,
           static_cast<void*>(vtbl), index, ms.perms, ms.path.c_str(), cur, mf.perms,
           mf.path.c_str());
  *info = buf;
  // r-x is accepted too: another hooker may have mprotect'ed the page that way.
  if (ms.perms[0] != 'r' || ms.perms[1] == 'w' || ms.path.empty() || ms.path[0] != '/') {
    *why = "vtable slot is not in a readonly file-backed image mapping";
    return false;
  }
  if (mf.perms[0] != 'r' || mf.perms[2] != 'x' || mf.path.empty()) {
    *why = "slot target is not in an executable image mapping";
    return false;
  }
  if (module && !(PathHasBasename(ms.path.c_str(), module) &&
                  PathHasBasename(mf.path.c_str(), module))) {
    *why = std::string("vtable or target is not in ") + module;
    return false;
  }
  *slotOut = slot;
  *curOut = cur;
  return true;
}

}  // namespace recline
