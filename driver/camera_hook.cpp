#include "camera_hook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "pose_math.h"
#include "recline_shm.h"
#include "slot_patch.h"

namespace recline {

namespace {

// vrserver's camera host asks the HMD driver for this component version.
constexpr const char* kCameraComponentVersion = "IVRCameraComponent_003";
// IVRCameraComponent_003 slot 8: const CameraVideoStreamFrame_t*
// GetVideoStreamFrame(). Same index as the last public version (_002); vrserver
// reads the returned frame's +0x14 (buffer index) and +0x58/+0x60 (server
// capture time/ticks), matching the public CameraVideoStreamFrame_t layout,
// which puts DriverPose_t m_RawTrackedDevicePose at +0x78.
constexpr size_t kGetFrameSlot = 8;
constexpr size_t kFramePoseOffset = 0x78;
constexpr double kHookGiveUpSeconds = 60.0;

using AddedFn = bool (*)(vr::IVRServerDriverHost*, const char*, vr::ETrackedDeviceClass,
                         vr::ITrackedDeviceServerDriver*);
using GetFrameFn = const void* (*)(void*);

std::atomic<bool> g_disabled{false};
std::atomic<AddedFn> g_prevAdded{nullptr};
void** g_addedSlot = nullptr;
std::atomic<vr::ITrackedDeviceServerDriver*> g_hmdDriver{nullptr};

std::atomic<GetFrameFn> g_origGetFrame{nullptr};
void** g_frameSlot = nullptr;
std::atomic<ReadRFn> g_readR{nullptr};
std::atomic<bool> g_loggedFirst{false};
std::function<void(const std::string&)> g_log;  // set before the frame hook is installed

bool AddedHook(vr::IVRServerDriverHost* self, const char* serial, vr::ETrackedDeviceClass cls,
               vr::ITrackedDeviceServerDriver* drv) {
  if (!g_disabled.load(std::memory_order_relaxed) && cls == vr::TrackedDeviceClass_HMD && drv)
    g_hmdDriver.store(drv, std::memory_order_release);
  return g_prevAdded.load(std::memory_order_acquire)(self, serial, cls, drv);
}

// ---------------------------------------------------------------------------
// Frame pose correction. vrserver's camera thread calls GetVideoStreamFrame;
// the frame buffers are the driver's and get reused. We rewrite the pose's
// world-from-driver in place, remembering per buffer what we wrote, so a buffer
// the driver hands back without refreshing its pose is never transformed twice.

struct Entry {
  const void* frame = nullptr;
  vr::HmdQuaternion_t origQ{1, 0, 0, 0}, wroteQ{1, 0, 0, 0};
  double origT[3] = {0, 0, 0}, wroteT[3] = {0, 0, 0};
};
std::mutex g_mu;
Entry g_entries[16];
size_t g_nextEntry = 0;
std::atomic<bool> g_anyEntry{false};

bool UnitQuat(const vr::HmdQuaternion_t& q) {
  const double n = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
  return std::isfinite(n) && std::fabs(n - 1.0) < 1e-3;
}

bool PlausiblePose(const vr::DriverPose_t& p) {
  if (!p.poseIsValid || !UnitQuat(p.qWorldFromDriverRotation) || !UnitQuat(p.qRotation))
    return false;
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(p.vecWorldFromDriverTranslation[i]) ||
        std::fabs(p.vecWorldFromDriverTranslation[i]) > 1000 || !std::isfinite(p.vecPosition[i]) ||
        std::fabs(p.vecPosition[i]) > 1000)
      return false;
  return true;
}

bool SameQ(const vr::HmdQuaternion_t& a, const vr::HmdQuaternion_t& b) {
  return a.w == b.w && a.x == b.x && a.y == b.y && a.z == b.z;
}
bool SameT(const double* a, const double* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }

void FixFrame(void* frame) {
  auto* p = reinterpret_cast<vr::DriverPose_t*>(static_cast<char*>(frame) + kFramePoseOffset);
  if (!PlausiblePose(*p)) return;  // unexpected layout or no pose: leave untouched
  Rigid R;
  const ReadRFn readR = g_readR.load(std::memory_order_acquire);
  const bool active = readR && readR(&R);
  if (!active && !g_anyEntry.load(std::memory_order_relaxed)) return;

  std::lock_guard<std::mutex> lk(g_mu);
  Entry* e = nullptr;
  for (Entry& x : g_entries)
    if (x.frame == frame) e = &x;
  if (!e) {
    if (!active) return;
    e = &g_entries[g_nextEntry++ % (sizeof g_entries / sizeof g_entries[0])];
    *e = Entry{};
    e->frame = frame;
    g_anyEntry.store(true, std::memory_order_relaxed);
  } else if (SameQ(p->qWorldFromDriverRotation, e->wroteQ) &&
             SameT(p->vecWorldFromDriverTranslation, e->wroteT)) {
    // Not refreshed by the driver since we wrote it: start from the original.
    p->qWorldFromDriverRotation = e->origQ;
    for (int i = 0; i < 3; ++i) p->vecWorldFromDriverTranslation[i] = e->origT[i];
  }
  e->origQ = p->qWorldFromDriverRotation;
  for (int i = 0; i < 3; ++i) e->origT[i] = p->vecWorldFromDriverTranslation[i];
  if (active) ApplyWorldTransform(R, p);
  e->wroteQ = p->qWorldFromDriverRotation;
  for (int i = 0; i < 3; ++i) e->wroteT[i] = p->vecWorldFromDriverTranslation[i];

  if (active && !g_loggedFirst.exchange(true) && g_log) {
    char buf[256];
    snprintf(buf, sizeof buf,
             "camera frame poses corrected (frame %p, world-from-driver q %.3f %.3f %.3f %.3f)",
             frame, e->origQ.w, e->origQ.x, e->origQ.y, e->origQ.z);
    g_log(buf);
  }
}

const void* GetFrameHook(void* self) {
  const void* f = g_origGetFrame.load(std::memory_order_acquire)(self);
  if (f && !g_disabled.load(std::memory_order_relaxed)) FixFrame(const_cast<void*>(f));
  return f;
}

// Same module check for two code/data addresses (the component's frame
// function must belong to the HMD driver's own module).
bool SameModule(const void* a, const void* b) {
  Mapping ma, mb;
  return FindMapping(reinterpret_cast<uintptr_t>(a), &ma) &&
         FindMapping(reinterpret_cast<uintptr_t>(b), &mb) && !ma.path.empty() &&
         ma.path == mb.path;
}

}  // namespace

bool InstallDeviceAddedHook(vr::IVRServerDriverHost* host, std::string* why) {
  void** slot;
  void* cur;
  std::string info;
  const size_t idx = VirtualSlotIndex(&vr::IVRServerDriverHost::TrackedDeviceAdded);
  if (!LocateVtableSlot(host, idx, nullptr, &slot, &cur, why, &info)) return false;
  if (cur == reinterpret_cast<void*>(&AddedHook)) {
    *why = "TrackedDeviceAdded hook already installed";
    return false;
  }
  g_prevAdded.store(reinterpret_cast<AddedFn>(cur), std::memory_order_release);
  std::string err;
  if (!PatchSlot(slot, cur, reinterpret_cast<void*>(&AddedHook), &err)) {
    *why = "patching TrackedDeviceAdded failed: " + err;
    return false;
  }
  g_addedSlot = slot;
  return true;
}

void MaybeHookCamera(bool hmdActive, ReadRFn readR,
                     const std::function<void(const std::string&)>& log) {
  static bool done = false;
  static double firstCall = 0;
  if (done || g_disabled.load()) return;
  const double now = MonotonicSeconds();
  if (firstCall == 0) firstCall = now;

  vr::ITrackedDeviceServerDriver* drv = g_hmdDriver.load(std::memory_order_acquire);
  if (!drv || !hmdActive) {
    if (now - firstCall > kHookGiveUpSeconds) {
      log(drv ? "camera hook: HMD never became active; passthrough camera poses not corrected"
              : "camera hook: no HMD device captured (added before recline loaded?); "
                "passthrough camera poses not corrected");
      done = true;
    }
    return;
  }
  done = true;  // one attempt

  void* comp = drv->GetComponent(kCameraComponentVersion);
  if (!comp) {
    log(std::string("camera hook: HMD has no ") + kCameraComponentVersion);
    return;
  }
  void** slot;
  void* cur;
  std::string why, info;
  if (!LocateVtableSlot(comp, kGetFrameSlot, nullptr, &slot, &cur, &why, &info)) {
    log("camera hook: " + why + (info.empty() ? "" : " (" + info + ")"));
    return;
  }
  if (!SameModule(cur, *reinterpret_cast<void**>(drv))) {
    log("camera hook: GetVideoStreamFrame is not in the HMD driver's module; not hooking");
    return;
  }
  g_log = log;
  g_readR.store(readR, std::memory_order_release);
  g_origGetFrame.store(reinterpret_cast<GetFrameFn>(cur), std::memory_order_release);
  std::string err;
  if (!PatchSlot(slot, cur, reinterpret_cast<void*>(&GetFrameHook), &err)) {
    log("camera hook: patch failed: " + err);
    return;
  }
  g_frameSlot = slot;
  log("camera hook: hooked " + std::string(kCameraComponentVersion) + " GetVideoStreamFrame" +
      (err.empty() ? "" : "; " + err));
}

void DisableCameraHooks() {
  g_disabled.store(true, std::memory_order_release);
  std::string err;
  if (g_frameSlot && __atomic_load_n(g_frameSlot, __ATOMIC_ACQUIRE) ==
                         reinterpret_cast<void*>(&GetFrameHook))
    PatchSlot(g_frameSlot, reinterpret_cast<void*>(&GetFrameHook),
              reinterpret_cast<void*>(g_origGetFrame.load()), &err);
  if (g_addedSlot && __atomic_load_n(g_addedSlot, __ATOMIC_ACQUIRE) ==
                         reinterpret_cast<void*>(&AddedHook))
    PatchSlot(g_addedSlot, reinterpret_cast<void*>(&AddedHook),
              reinterpret_cast<void*>(g_prevAdded.load()), &err);
}

}  // namespace recline
