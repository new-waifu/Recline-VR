// recline_xrshim: a shared library loaded (via LD_PRELOAD) into the Frame's
// XRService process ONLY, by the driver's targeted spawn hook. XRService draws
// the passthrough ("constructed room view") camera image onto a 3D room mesh
// whose pose it writes as the vrpaths property /meshData/rawFromMeshPose. That
// pose is in the pre-R "raw" driver-world frame, but the compositor renders the
// mesh with the HMD pose, which the driver's recline transform R has already
// rotated. The two disagree, so passthrough no longer lines up with reality.
//
// This shim closes the gap at the source: it hooks IVRPaths::WritePathBatch in
// XRService and rewrites rawFromMeshPose to R ∘ pose as it is written, using
// the same R the driver reads from /recline_v1. Everything else passes through
// untouched. It fails safe: if anything about the process, the interface
// version, or the shared memory is not exactly as expected, it does nothing and
// forwards every call unchanged.
//
// It relies on two public exported symbols (VR_GetGenericInterface, VRPaths)
// plus two internal-but-versioned details of this SteamVR build: the
// IVRPaths_002 vtable layout (slot 1 = WritePathBatch) and the PathWrite_t
// struct layout. Both are guarded; a mismatch disables the shim.

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "recline_math.h"
#include "recline_shm.h"
#include "slot_patch.h"

using namespace recline;

namespace {

#define SHIM_EXPORT extern "C" __attribute__((visibility("default")))

// The exact executable this shim is allowed to run in. Anywhere else it is a
// pure forwarder (defence in depth; the driver only ever preloads it here).
constexpr const char* kXRServiceSuffix = "/drivers/cv/bin/linuxarm64/XRService";
constexpr const char* kVrclientModule = "vrclient.so";
constexpr const char* kRawFromMeshPose = "/meshData/rawFromMeshPose";

std::atomic<bool> g_active{false};       // guard passed: this is XRService
std::atomic<bool> g_installed{false};    // WritePathBatch hooked
std::atomic<void*> g_vtbl{nullptr};      // the IVRPaths_002 vtable we validated

// ---------------------------------------------------------------------------
// Logging: append to ~/.local/share/recline-vr/xrshim.log, short, rate-limited.

FILE* g_log = nullptr;
void LogOpen() {
  const char* home = getenv("HOME");
  if (!home) return;
  char path[PATH_MAX];
  snprintf(path, sizeof path, "%s/.local/share/recline-vr/xrshim.log", home);
  g_log = fopen(path, "ae");
}
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
  if (!g_log) return;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  fprintf(g_log, "[xrshim] %s\n", buf);
  fflush(g_log);
}

// ---------------------------------------------------------------------------
// Read-only view of /recline_v1 (the driver creates and owns it; the shim only
// ever reads the StateBlock). Mapped PROT_READ; ReadState does atomic loads.

const ReclineShm* g_shm = nullptr;
double g_nextShmTry = 0;

const ReclineShm* OpenShmReadOnly() {
  int fd = shm_open(kShmName, O_RDONLY, 0);
  if (fd < 0) return nullptr;
  struct stat st {};
  if (fstat(fd, &st) != 0 || size_t(st.st_size) < kShmSize) {
    close(fd);
    return nullptr;
  }
  void* p = mmap(nullptr, kShmSize, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) return nullptr;
  auto* shm = static_cast<const ReclineShm*>(p);
  if (__atomic_load_n(&shm->magic, __ATOMIC_ACQUIRE) != kMagic ||
      __atomic_load_n(&shm->version, __ATOMIC_RELAXED) != kVersion) {
    munmap(p, kShmSize);
    return nullptr;
  }
  return shm;
}

// Current R from the StateBlock. Returns false (identity) if the segment is
// absent (retry at most once a second) or R is not yet active/sane.
bool CurrentR(Rigid* out) {
  if (!g_shm) {
    const double now = MonotonicSeconds();
    if (now < g_nextShmTry) return false;
    g_nextShmTry = now + 1.0;
    g_shm = OpenShmReadOnly();
    if (!g_shm) return false;
    Log("opened /recline_v1 (read-only)");
  }
  StateSnapshot s;
  if (!ReadState(g_shm->state, &s)) return false;
  if (!(s.flags & FLAG_TRANSFORM_ACTIVE)) return false;
  Rigid r{{s.R_q[0], s.R_q[1], s.R_q[2], s.R_q[3]}, {s.R_t[0], s.R_t[1], s.R_t[2]}};
  if (IsIdentity(r) || !IsSaneRigid(r)) return false;
  r.q = Normalize(r.q);
  *out = r;
  return true;
}

// ---------------------------------------------------------------------------
// PathWrite_t: the element of the array passed to WritePathBatch. Layout is an
// internal detail of this build (stride 0x38); every field the hook touches is
// validated (tag + buffer size) before use, and a wrong layout simply fails to
// match the rawFromMeshPose entry, so the shim stays a pass-through.
struct PathWrite {
  uint64_t ulPath;       // 0x00 handle; if 0, pszPath is resolved instead
  uint32_t writeType;    // 0x08
  uint32_t setError;     // 0x0c
  void* pvBuffer;        // 0x10
  uint32_t unBufferSize; // 0x18
  uint32_t unTag;        // 0x1c  PropertyTypeTag_t
  uint32_t errorOut;     // 0x20
  uint32_t _pad24;       // 0x24
  const char* pszPath;   // 0x28
  uint16_t flags;        // 0x30
  uint16_t _pad32;
  uint32_t _pad34;
};
static_assert(sizeof(PathWrite) == 0x38, "PathWrite_t stride");
static_assert(offsetof(PathWrite, pvBuffer) == 0x10, "layout");
static_assert(offsetof(PathWrite, unBufferSize) == 0x18, "layout");
static_assert(offsetof(PathWrite, unTag) == 0x1c, "layout");
static_assert(offsetof(PathWrite, pszPath) == 0x28, "layout");

constexpr uint32_t kHmdMatrix34Tag = 20;  // k_unHmdMatrix34PropertyTag

// HmdMatrix34_t is row-major float[3][4]: rotation m[r][c] (c<3), translation
// m[r][3]. Compute M' = [Rrot·Mrot | Rrot·Mt + Rt] in place.
void ApplyRToMatrix34(const Rigid& R, float m[3][4]) {
  // Columns of the incoming rotation (c0..c2) and its translation (t).
  Vec3 c0{m[0][0], m[1][0], m[2][0]};
  Vec3 c1{m[0][1], m[1][1], m[2][1]};
  Vec3 c2{m[0][2], m[1][2], m[2][2]};
  Vec3 t{m[0][3], m[1][3], m[2][3]};
  Vec3 r0 = Rotate(R.q, c0), r1 = Rotate(R.q, c1), r2 = Rotate(R.q, c2);
  Vec3 rt = Rotate(R.q, t) + R.t;
  m[0][0] = float(r0.x); m[1][0] = float(r0.y); m[2][0] = float(r0.z);
  m[0][1] = float(r1.x); m[1][1] = float(r1.y); m[2][1] = float(r1.z);
  m[0][2] = float(r2.x); m[1][2] = float(r2.y); m[2][2] = float(r2.z);
  m[0][3] = float(rt.x); m[1][3] = float(rt.y); m[2][3] = float(rt.z);
}

// ---------------------------------------------------------------------------
// The WritePathBatch hook. Signature mirrors the vrclient method:
// int WritePathBatch(void* self, uint64_t root, PathWrite* w, uint32_t n).
using WriteFn = uint32_t (*)(void*, uint64_t, PathWrite*, uint32_t);
// Slot 2: StringToHandle(PathHandle_t* out, const char* path) -> error code.
using StringToHandleFn = uint32_t (*)(void*, uint64_t*, const char*);
std::atomic<WriteFn> g_origWrite{nullptr};
std::atomic<StringToHandleFn> g_stringToHandle{nullptr};

// XRService writes by handle (ulPath), resolved once via StringToHandle; the
// string is only consulted when ulPath is 0. Resolved lazily on the first hook
// call, outside any vrclient lock.
std::atomic<uint64_t> g_hRaw{0};
std::atomic<bool> g_hRawTried{false};

void ResolveRawHandle(void* self) {
  if (g_hRawTried.exchange(true, std::memory_order_acq_rel)) return;
  const StringToHandleFn fn = g_stringToHandle.load(std::memory_order_acquire);
  uint64_t h = 0;
  const uint32_t err = fn ? fn(self, &h, kRawFromMeshPose) : ~0u;
  g_hRaw.store(err == 0 ? h : 0, std::memory_order_release);
  Log("StringToHandle(%s) = 0x%llx (err %u)", kRawFromMeshPose, (unsigned long long)h, err);
}

std::atomic<uint64_t> g_hits{0};
double g_nextHitLog = 0;
std::atomic<int> g_matrixWritesLogged{0};

uint32_t HookWritePathBatch(void* self, uint64_t root, PathWrite* w, uint32_t n) {
  const WriteFn orig = g_origWrite.load(std::memory_order_acquire);
  if (!g_active.load(std::memory_order_relaxed) || !orig || n == 0 || n > 64)
    return orig ? orig(self, root, w, n) : 0;
  ResolveRawHandle(self);
  const uint64_t hRaw = g_hRaw.load(std::memory_order_acquire);

  // Diagnostics: the first few matrix writes, so a mismatch is visible.
  for (uint32_t i = 0; i < n; ++i) {
    if (w[i].unTag != kHmdMatrix34Tag || w[i].unBufferSize != 48) continue;
    if (g_matrixWritesLogged.fetch_add(1, std::memory_order_relaxed) >= 8) break;
    Log("matrix write: root 0x%llx path 0x%llx str %s%s", (unsigned long long)root,
        (unsigned long long)w[i].ulPath, w[i].pszPath ? w[i].pszPath : "(null)",
        hRaw && w[i].ulPath == hRaw ? " [rawFromMeshPose]" : "");
  }

  Rigid R;
  if (!CurrentR(&R)) return orig(self, root, w, n);  // identity or no shm

  bool hit = false;
  PathWrite copy[64];
  float mats[64][3][4];
  for (uint32_t i = 0; i < n; ++i) {
    copy[i] = w[i];
    const bool pathMatch =
        (hRaw && w[i].ulPath == hRaw) ||
        (w[i].ulPath == 0 && w[i].pszPath && strcmp(w[i].pszPath, kRawFromMeshPose) == 0);
    const bool isRaw =
        pathMatch && w[i].unTag == kHmdMatrix34Tag && w[i].unBufferSize == 48 && w[i].pvBuffer;
    if (isRaw) {
      memcpy(mats[i], w[i].pvBuffer, 48);
      ApplyRToMatrix34(R, mats[i]);
      copy[i].pvBuffer = mats[i];
      hit = true;
    }
  }
  if (!hit) return orig(self, root, w, n);

  const uint32_t r = orig(self, root, copy, n);
  // Propagate per-entry error outputs back to the caller's buffer.
  for (uint32_t i = 0; i < n; ++i) {
    w[i].setError = copy[i].setError;
    w[i].errorOut = copy[i].errorOut;
  }
  const uint64_t hits = g_hits.fetch_add(1, std::memory_order_relaxed) + 1;
  const double now = MonotonicSeconds();
  if (now >= g_nextHitLog) {
    g_nextHitLog = now + 10.0;
    Log("rawFromMeshPose rewrites: %llu total", (unsigned long long)hits);
  }
  return r;
}

// ---------------------------------------------------------------------------
// Install: validate the IVRPaths_002 vtable and patch slot 1 once per process.

void Install(void* obj) {
  if (!obj || !g_active.load(std::memory_order_relaxed)) return;
  if (g_installed.load(std::memory_order_acquire)) return;

  // Match by path only (kRawFromMeshPose matching happens in the hook).
  void** vtbl = *reinterpret_cast<void***>(obj);
  void** slot;
  void* cur;
  std::string why, info;
  // Slot 1 of IVRPaths_002 = WritePathBatch; validate it lives in vrclient.so.
  if (!LocateVtableSlot(obj, 1, kVrclientModule, &slot, &cur, &why, &info)) {
    Log("not hooking: %s (%s)", why.c_str(), info.c_str());
    return;
  }
  if (cur == reinterpret_cast<void*>(&HookWritePathBatch)) return;

  // Slot 2 (StringToHandle) must also live in vrclient.so; take it before
  // patching slot 1.
  void** slot2;
  void* stringToHandle;
  if (!LocateVtableSlot(obj, 2, kVrclientModule, &slot2, &stringToHandle, &why, &info)) {
    Log("not hooking: StringToHandle: %s (%s)", why.c_str(), info.c_str());
    return;
  }
  g_stringToHandle.store(reinterpret_cast<StringToHandleFn>(stringToHandle),
                         std::memory_order_release);
  g_origWrite.store(reinterpret_cast<WriteFn>(cur), std::memory_order_release);
  std::string err;
  if (!PatchSlot(slot, cur, reinterpret_cast<void*>(&HookWritePathBatch), &err)) {
    Log("patch failed: %s", err.c_str());
    return;
  }
  g_vtbl.store(vtbl, std::memory_order_release);
  g_installed.store(true, std::memory_order_release);
  Log("hooked WritePathBatch (vtbl=%p, orig=%p)%s%s", (void*)vtbl, cur,
      err.empty() ? "" : "; ", err.c_str());
}

// ---------------------------------------------------------------------------
// Constructor: guard on the executable, drop our own LD_PRELOAD entry so no
// child of XRService inherits it, and open the log.

void StripSelfFromPreload() {
  const char* pre = getenv("LD_PRELOAD");
  if (!pre) return;
  // Our path contains "recline_xrshim"; rebuild LD_PRELOAD without any token
  // containing it. Tokens are separated by ':' or ' '.
  std::string out;
  const char* p = pre;
  while (*p) {
    const char* sep = p;
    while (*sep && *sep != ':' && *sep != ' ') ++sep;
    std::string tok(p, sep - p);
    if (!tok.empty() && tok.find("recline_xrshim") == std::string::npos) {
      if (!out.empty()) out += ':';
      out += tok;
    }
    p = (*sep) ? sep + 1 : sep;
  }
  if (out.empty())
    unsetenv("LD_PRELOAD");
  else
    setenv("LD_PRELOAD", out.c_str(), 1);
}

__attribute__((constructor)) void Init() {
  char exe[PATH_MAX];
  const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0) return;
  exe[n] = '\0';
  const size_t le = strlen(exe), ls = strlen(kXRServiceSuffix);
  if (le < ls || strcmp(exe + le - ls, kXRServiceSuffix) != 0) return;  // not XRService

  g_active.store(true, std::memory_order_release);
  LogOpen();
  StripSelfFromPreload();
  Log("active in %s (pid %d)", exe, (int)getpid());
}

}  // namespace

// ---------------------------------------------------------------------------
// Interposed exports. Each forwards to the real implementation via RTLD_NEXT,
// then, when active, installs the hook from the returned IVRPaths object.

SHIM_EXPORT void* VR_GetGenericInterface(const char* version, void* err) {
  using Fn = void* (*)(const char*, void*);
  static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "VR_GetGenericInterface"));
  void* obj = real ? real(version, err) : nullptr;
  if (obj && g_active.load(std::memory_order_relaxed) && version) {
    if (strcmp(version, "IVRPaths_002") == 0)
      Install(obj);
    else if (strncmp(version, "IVRPaths_", 9) == 0)
      Log("unsupported %s requested; not hooking", version);
  }
  return obj;
}

SHIM_EXPORT void* VRPaths() {
  using Fn = void* (*)();
  static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "VRPaths"));
  void* obj = real ? real() : nullptr;
  // VRPaths()'s return type is unversioned: only hook it once we have already
  // validated the same vtable through an explicit IVRPaths_002 request.
  if (obj && g_active.load(std::memory_order_relaxed) &&
      *reinterpret_cast<void**>(obj) == g_vtbl.load(std::memory_order_acquire))
    Install(obj);
  return obj;
}
