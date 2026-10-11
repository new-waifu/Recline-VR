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
// the same R the driver reads from /recline_v1. XRService also publishes each
// passthrough camera frame's world-from-camera pose (same pre-R frame) through
// IVRCameraPassthroughInternal; that pose gets the same R, so the camera image
// is projected onto the mesh from where the camera really is. Everything else
// passes through untouched. It fails safe: if anything about the process, the
// interface version, or the shared memory is not exactly as expected, it does
// nothing and forwards every call unchanged. docs/DESIGN.md ("Passthrough")
// explains why the fix lives here and how injection is kept out of games.
//
// It relies on two public exported symbols (VR_GetGenericInterface, VRPaths)
// plus internal-but-versioned details of this SteamVR build: the IVRPaths_002
// vtable layout (slot 1 = WritePathBatch, slot 2 = StringToHandle), the
// PathWrite_t struct layout, and IVRCameraPassthroughInternal_001 slot 5 with
// its frame-info layout. All are guarded; a mismatch leaves calls unchanged.

#include <atomic>
#include <cmath>
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
// Logging: append to ~/.local/share/recline-vr/xrshim.log; a few one-shot lines
// (install, validation, first rewrite), never per call.

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

double g_nextShmTry = 0;  // guarded by Shm()'s opener flag

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

// The mapped segment, opened lazily (retry at most once a second). Called from
// both the mesh and the camera hook threads; one opener at a time.
const ReclineShm* Shm() {
  static std::atomic<const ReclineShm*> shm{nullptr};
  static std::atomic<bool> opening{false};
  if (const ReclineShm* s = shm.load(std::memory_order_acquire)) return s;
  if (opening.exchange(true, std::memory_order_acquire)) return nullptr;
  const double now = MonotonicSeconds();
  const ReclineShm* s = nullptr;
  if (now >= g_nextShmTry) {
    g_nextShmTry = now + 1.0;
    s = OpenShmReadOnly();
    if (s) {
      shm.store(s, std::memory_order_release);
      Log("opened /recline_v1 (read-only)");
    }
  }
  opening.store(false, std::memory_order_release);
  return s;
}

// Current R from the StateBlock. Returns false (identity) if the segment is
// absent or R is not yet active/sane.
bool CurrentR(Rigid* out) {
  const ReclineShm* shm = Shm();
  if (!shm) return false;
  StateSnapshot s;
  if (!ReadState(shm->state, &s)) return false;
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

std::atomic<bool> g_loggedFirstHit{false};

uint32_t HookWritePathBatch(void* self, uint64_t root, PathWrite* w, uint32_t n) {
  const WriteFn orig = g_origWrite.load(std::memory_order_acquire);
  if (!g_active.load(std::memory_order_relaxed) || !orig || n == 0 || n > 64)
    return orig ? orig(self, root, w, n) : 0;
  ResolveRawHandle(self);
  const uint64_t hRaw = g_hRaw.load(std::memory_order_acquire);

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
  if (!g_loggedFirstHit.exchange(true, std::memory_order_relaxed))
    Log("rewriting rawFromMeshPose for the recline transform");
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
// Passthrough camera frames. XRService owns the cameras and publishes each
// frame's metadata to the compositor through IVRCameraPassthroughInternal_001
// slot 5: (cameraType, cameraIndex, bufferIndex, const info*), which copies
// kCameraInfoSize bytes of `info` into the shared passthrough state. The
// world-from-camera pose in it (row-major 3x4 at +0x30) is in the same pre-R
// frame as rawFromMeshPose, so it gets the same R; otherwise the compositor
// projects the camera image onto the (corrected) mesh from the wrong place.
//
// Both numbers are internal to this SteamVR build, so neither is trusted:
//  - at install, vrclient's slot-5 code must copy exactly kCameraInfoSize bytes
//    (its memcpy size immediate is read back from the instruction stream);
//  - before the first rewrite, the matrix must behave like a head-mounted
//    camera's pose: C = inv(H)·M, with H the pre-R head pose the driver
//    publishes, must stay constant (cameras are rigidly mounted) and small in
//    translation while the head turns by at least kValidateMinHeadTravelDeg.
// Any mismatch disables the camera hook for the life of the process.

constexpr const char* kCameraPassthroughVersion = "IVRCameraPassthroughInternal_001";
constexpr size_t kSetFrameInfoSlot = 5;
constexpr size_t kCameraInfoSize = 0xf8;
constexpr size_t kWorldFromCameraOffset = 0x30;

constexpr int kValidateSamples = 30;            // frames per validation window
constexpr int kValidateMinGood = 20;            // consistent frames needed in a window
constexpr double kValidateMaxSpreadDeg = 4.0;   // C drift allowed (head motion vs H latency)
constexpr double kValidateMaxOffsetM = 0.25;    // camera within this of the head
constexpr double kValidateMinHeadTravelDeg = 10.0;
constexpr double kValidateMaxHeadAgeS = 0.05;   // H must be this fresh

enum CameraState { kValidating, kActive, kDisabled };

using SetFrameInfoFn = uint32_t (*)(void*, uint32_t, uint32_t, uint32_t, const void*);
std::atomic<SetFrameInfoFn> g_origSetFrameInfo{nullptr};
std::atomic<bool> g_cameraInstalled{false};
std::atomic<int> g_cameraState{kValidating};
std::atomic<bool> g_loggedFirstCamera{false};

bool IsRotation34(const float m[3][4]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double d = 0;
      for (int k = 0; k < 3; ++k) d += double(m[k][i]) * m[k][j];  // columns orthonormal
      if (!(std::fabs(d - (i == j ? 1.0 : 0.0)) < 1e-3)) return false;
    }
  for (int r = 0; r < 3; ++r)
    if (!std::isfinite(m[r][3]) || std::fabs(m[r][3]) > 1000) return false;
  return true;
}

Rigid RigidFromMatrix34(const float m[3][4]) {
  struct { float m[3][4]; } mm;
  memcpy(mm.m, m, sizeof mm.m);
  return FromMatrix34(mm);
}

void DisableCamera(const char* why) {
  if (g_cameraState.exchange(kDisabled) != kDisabled)
    Log("camera pose correction disabled: %s", why);
}

// Validation state: only touched under g_validateMu (frames may in principle
// arrive on more than one thread; a busy lock just skips the sample).
std::atomic_flag g_validateMu = ATOMIC_FLAG_INIT;
// Each physical camera has its own mount, so C is tracked per (type, camera).
struct {
  int samples = 0, good = 0, windows = 0, invalid = 0;
  bool haveC[4] = {};
  Rigid refC[4], refH{{1, 0, 0, 0}, {0, 0, 0}};
  double headTravelDeg = 0, maxSpreadDeg = 0, maxOffsetM = 0;
} g_val;

void ValidateSample(uint32_t type, uint32_t cam, const float m[3][4]) {
  if (g_validateMu.test_and_set(std::memory_order_acquire)) return;
  struct Unlock { ~Unlock() { g_validateMu.clear(std::memory_order_release); } } unlock;
  if (g_cameraState.load() != kValidating || cam > 1) return;
  const int key = (type ? 2 : 0) + int(cam);

  if (!IsRotation34(m)) {
    if (++g_val.invalid >= kValidateSamples) DisableCamera("frame info has no rotation at the expected offset");
    return;
  }
  const ReclineShm* shm = Shm();
  HmdSnapshot h;
  if (!shm || !ReadHmd(shm->hmd, &h) || MonotonicSeconds() - h.time > kValidateMaxHeadAgeS)
    return;  // no fresh head pose to compare against: wait
  const Rigid H{Normalize(Quat{h.q[0], h.q[1], h.q[2], h.q[3]}), {h.t[0], h.t[1], h.t[2]}};
  const Rigid C = Compose(Inverse(H), RigidFromMatrix34(m));

  // References persist across windows: C must stay constant for the whole
  // validation, and head travel accumulates from the first sample.
  if (g_val.windows == 0 && g_val.samples == 0) g_val.refH = H;
  if (g_val.samples == 0) g_val.good = 0;
  if (!g_val.haveC[key]) {
    g_val.refC[key] = C;
    g_val.haveC[key] = true;
  }
  ++g_val.samples;
  const double spread = Deg(AngleBetween(C.q, g_val.refC[key].q));
  if (spread <= kValidateMaxSpreadDeg && Length(C.t) <= kValidateMaxOffsetM) {
    ++g_val.good;
    if (spread > g_val.maxSpreadDeg) g_val.maxSpreadDeg = spread;
    if (Length(C.t) > g_val.maxOffsetM) g_val.maxOffsetM = Length(C.t);
    const double travel = Deg(AngleBetween(H.q, g_val.refH.q));
    if (travel > g_val.headTravelDeg) g_val.headTravelDeg = travel;
  }
  if (g_val.samples < kValidateSamples) return;

  // End of a window.
  g_val.samples = 0;
  ++g_val.windows;
  if (g_val.good < kValidateMinGood) {
    char why[160];
    snprintf(why, sizeof why,
             "camera pose not rigidly head-mounted (%d/%d consistent frames)", g_val.good,
             kValidateSamples);
    DisableCamera(why);
  } else if (g_val.headTravelDeg >= kValidateMinHeadTravelDeg) {
    g_cameraState.store(kActive);
    Log("camera pose layout validated: cameras within %.1f cm of the head, spread %.1f deg "
        "over %.0f deg of head motion",
        g_val.maxOffsetM * 100, g_val.maxSpreadDeg, g_val.headTravelDeg);
  }
  // else: consistent so far but the head hasn't turned enough to tell a
  // head-mounted pose from a fixed one; keep validating (frames pass through
  // uncorrected meanwhile).
}

uint32_t HookSetFrameInfo(void* self, uint32_t type, uint32_t cam, uint32_t buf,
                          const void* info) {
  const SetFrameInfoFn orig = g_origSetFrameInfo.load(std::memory_order_acquire);
  const int state = g_cameraState.load(std::memory_order_relaxed);
  if (!info || state == kDisabled) return orig(self, type, cam, buf, info);

  float m[3][4];
  memcpy(m, static_cast<const unsigned char*>(info) + kWorldFromCameraOffset, sizeof m);
  if (state == kValidating) {
    ValidateSample(type, cam, m);
    return orig(self, type, cam, buf, info);
  }

  Rigid R;
  if (!CurrentR(&R) || !IsRotation34(m)) return orig(self, type, cam, buf, info);
  alignas(16) unsigned char copy[kCameraInfoSize];
  memcpy(copy, info, sizeof copy);
  ApplyRToMatrix34(R, m);
  memcpy(copy + kWorldFromCameraOffset, m, sizeof m);
  if (!g_loggedFirstCamera.exchange(true, std::memory_order_relaxed))
    Log("rewriting passthrough camera poses for the recline transform");
  return orig(self, type, cam, buf, copy);
}

// The size vrclient's slot-5 function copies: the immediate of the first
// `mov x2, #imm` that is followed within a few instructions by a `bl` (the
// memcpy). -1 if no such pair is found in the function's first instructions.
long CopySizeOf(const void* fn) {
#if defined(__aarch64__)
  Mapping map;
  if (!FindMapping(reinterpret_cast<uintptr_t>(fn), &map)) return -1;
  const auto* code = static_cast<const uint32_t*>(fn);
  const size_t avail = (map.end - reinterpret_cast<uintptr_t>(fn)) / 4;
  const size_t n = avail < 96 ? avail : 96;
  for (size_t i = 0; i < n; ++i) {
    if ((code[i] & 0xffe0001fu) != 0xd2800002u) continue;  // movz x2, #imm16 (lsl 0)
    for (size_t j = i + 1; j < n && j <= i + 6; ++j)
      if ((code[j] & 0xfc000000u) == 0x94000000u) return long((code[i] >> 5) & 0xffff);  // bl
  }
#else
  (void)fn;
#endif
  return -1;
}

void InstallCamera(void* obj) {
  if (!obj || !g_active.load(std::memory_order_relaxed)) return;
  if (g_cameraInstalled.exchange(true, std::memory_order_acq_rel)) return;
  void** slot;
  void* cur;
  std::string why, info;
  if (!LocateVtableSlot(obj, kSetFrameInfoSlot, kVrclientModule, &slot, &cur, &why, &info)) {
    Log("not hooking camera frames: %s (%s)", why.c_str(), info.c_str());
    return;
  }
  if (cur == reinterpret_cast<void*>(&HookSetFrameInfo)) return;
  const long size = CopySizeOf(cur);
  if (size != long(kCameraInfoSize)) {
    Log("not hooking camera frames: slot %zu copies %ld bytes, expected %zu", kSetFrameInfoSlot,
        size, kCameraInfoSize);
    return;
  }
  g_origSetFrameInfo.store(reinterpret_cast<SetFrameInfoFn>(cur), std::memory_order_release);
  std::string err;
  if (!PatchSlot(slot, cur, reinterpret_cast<void*>(&HookSetFrameInfo), &err)) {
    Log("camera patch failed: %s", err.c_str());
    return;
  }
  Log("hooked %s slot %zu (orig=%p); validating before first rewrite%s%s",
      kCameraPassthroughVersion, kSetFrameInfoSlot, cur, err.empty() ? "" : "; ", err.c_str());
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
    else if (strcmp(version, kCameraPassthroughVersion) == 0)
      InstallCamera(obj);
    else if (strncmp(version, "IVRPaths_", 9) == 0 ||
             strncmp(version, "IVRCameraPassthroughInternal_", 29) == 0)
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
