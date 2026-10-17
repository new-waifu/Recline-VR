// driver_recline: OpenVR server driver that adds no devices. It intercepts
// IVRServerDriverHost::TrackedDevicePoseUpdated through the (shared) host
// vtable and left-multiplies every device's world-from-driver transform by a
// rigid transform R, so the whole tracked world can be pitched/rolled, not just
// yawed. R is controlled over the /recline_v1 shared-memory protocol. Frame
// builds (RECLINE_FRAME) also install the targeted spawn injection
// (spawn_inject.h) that loads the passthrough shim into XRService, whose poses
// never reach this hook.
// docs/DESIGN.md explains why this approach (the official chaperone API can
// only yaw) and the safety rules the hook follows.

#include <openvr_driver.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "auto_reset.h"
#include "layer_hook.h"
#include "platform.h"
#include "pose_hook.h"
#include "pose_math.h"
#include "recline_math.h"
#include "recline_shm.h"
#include "recline_version.h"
#ifdef RECLINE_FRAME
#include "slot_patch.h"
#include "spawn_inject.h"
#endif

#ifdef _WIN32
#define RECLINE_EXPORT extern "C" __declspec(dllexport)
#define RECLINE_PRINTF(f, a)
#else
#define RECLINE_EXPORT extern "C" __attribute__((visibility("default")))
#define RECLINE_PRINTF(f, a) __attribute__((format(printf, f, a)))
#endif

namespace {

using namespace recline;

constexpr const char* kSection = "driver_recline";

// ---------------------------------------------------------------------------
// Logging (never from the hot path)

void Log(const char* fmt, ...) RECLINE_PRINTF(1, 2);
void Log(const char* fmt, ...) {
  char buf[1024];
  int n = snprintf(buf, sizeof buf, "[recline] ");
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + n, sizeof buf - n, fmt, ap);
  va_end(ap);
  if (vr::VRDriverLog()) vr::VRDriverLog()->Log(buf);
}

// ---------------------------------------------------------------------------
// Global hook state

std::atomic<PoseFn> g_prev{nullptr};
std::atomic<bool> g_disabled{false};
std::atomic<bool> g_hookInstalled{false};
std::atomic<uint32_t> g_hmdIndex{vr::k_unTrackedDeviceIndex_Hmd};
std::atomic<ReclineShm*> g_shm{nullptr};
std::atomic<uint64_t> g_hmdPoseCount{0};
std::atomic<bool> g_downstreamCheckEnabled{true};

// ---------------------------------------------------------------------------
// In-process R: two seqlocked copies, written one after the other by the
// command thread, so a bounded hot-path reader that loses on one copy can read
// the other (docs/DESIGN.md, "Hot-path reads of R").
struct alignas(64) LocalR {
  uint32_t seq;
  uint32_t identity;  // payload: 1 if R is identity (fast path)
  double q[4];
  double t[3];
};
LocalR g_R[2] = {{0, 1, {1, 0, 0, 0}, {0, 0, 0}}, {0, 1, {1, 0, 0, 0}, {0, 0, 0}}};

struct RSnapshot {
  uint32_t identity;
  Rigid r;
};

void WriteLocalR(const Rigid& r) {  // command thread only
  const uint32_t ident = IsIdentity(r) ? 1u : 0u;
  for (LocalR& b : g_R) {
    const uint32_t s = SeqWriteBeginSingle(b.seq);
    St(b.identity, ident);
    St(b.q[0], r.q.w); St(b.q[1], r.q.x); St(b.q[2], r.q.y); St(b.q[3], r.q.z);
    St(b.t[0], r.t.x); St(b.t[1], r.t.y); St(b.t[2], r.t.z);
    SeqWriteEnd(b.seq, s);
  }
}

// Hot path: bounded (16 attempts per copy), never blocks.
inline bool ReadLocalR(RSnapshot* o) {
  for (const LocalR& b : g_R) {
    RSnapshot s;
    const bool ok = SeqRead(b.seq, [&] {
      s.identity = Ld(b.identity);
      s.r.q = {Ld(b.q[0]), Ld(b.q[1]), Ld(b.q[2]), Ld(b.q[3])};
      s.r.t = {Ld(b.t[0]), Ld(b.t[1]), Ld(b.t[2])};
    }, 16);
    if (ok) {
      // Validate after the seqlock: consistency is not sanity.
      if (!s.identity && !IsSaneRigid(s.r)) return false;
      *o = s;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// HMD publish (multi-writer try-lock: skip if busy, never spin)

void PublishHmd(const vr::DriverPose_t& in) {
  const uint64_t count = g_hmdPoseCount.fetch_add(1, std::memory_order_relaxed) + 1;
  ReclineShm* shm = g_shm.load(std::memory_order_acquire);
  if (!shm) return;
  HmdBlock& b = shm->hmd;
  uint32_t s;
  if (!SeqTryLock(b.seq, &s)) return;
  if (in.poseIsValid) {
    const Rigid h = HeadFromDriverPose(in);
    St(b.q[0], h.q.w); St(b.q[1], h.q.x); St(b.q[2], h.q.y); St(b.q[3], h.q.z);
    St(b.t[0], h.t.x); St(b.t[1], h.t.y); St(b.t[2], h.t.z);
    St(b.time, MonotonicSeconds());
  }
  St(b.pose_count, count);
  SeqWriteEnd(b.seq, s);
}

// ---------------------------------------------------------------------------
// Downstream-conflict self-check, sampled ~1 Hz from the hook: compare what
// vrserver reports for the HMD with R ∘ H. A persistent mismatch means another
// hook modifies poses after ours.

constexpr double kCheckPosTolM = 0.002;
constexpr double kCheckRotTolRad = 0.25 * kPi / 180.0;
constexpr int kCheckPersist = 5;  // consecutive mismatches before flagging

std::atomic<double> g_nextCheckTime{0.0};
std::atomic<int> g_mismatchStreak{0};
std::atomic<bool> g_conflictDownstream{false};
// 0 unknown, 1 post-world-from-driver (check usable), 2 driver space (check dropped)
std::atomic<int> g_rawPoseSemantics{0};
std::atomic<double> g_lastCheckPosErr{0.0}, g_lastCheckRotErr{0.0};
std::atomic<uint32_t> g_checkCount{0};

void MaybeCheckDownstream(vr::IVRServerDriverHost* self, uint32_t id,
                          const vr::DriverPose_t& in, const RSnapshot& R) {
  if (!g_downstreamCheckEnabled.load(std::memory_order_relaxed)) return;
  if (g_rawPoseSemantics.load(std::memory_order_relaxed) == 2) return;
  if (!in.poseIsValid || id >= vr::k_unMaxTrackedDeviceCount) return;
  const double now = MonotonicSeconds();
  double next = g_nextCheckTime.load(std::memory_order_relaxed);
  if (now < next) return;
  if (!g_nextCheckTime.compare_exchange_strong(next, now + 1.0, std::memory_order_relaxed))
    return;  // another thread took this sample

  vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
  self->GetRawTrackedDevicePoses(0.0f, poses, id + 1);
  if (!poses[id].bPoseIsValid) return;
  const Rigid got = FromMatrix34(poses[id].mDeviceToAbsoluteTracking);

  const Rigid H = HeadFromDriverPose(in);
  const Rigid expected = R.identity ? H : Compose(R.r, H);
  const double posErr = Length(got.t - expected.t);
  const double rotErr = AngleBetween(got.q, expected.q);
  g_lastCheckPosErr.store(posErr, std::memory_order_relaxed);
  g_lastCheckRotErr.store(rotErr, std::memory_order_relaxed);
  g_checkCount.fetch_add(1, std::memory_order_relaxed);

  if (posErr <= kCheckPosTolM && rotErr <= kCheckRotTolRad) {
    g_rawPoseSemantics.store(1, std::memory_order_relaxed);
    g_mismatchStreak.store(0, std::memory_order_relaxed);
    g_conflictDownstream.store(false, std::memory_order_relaxed);
    return;
  }
  // Does the API return poses before world-from-driver? Then this check can't
  // see downstream modifications at all: drop it rather than report false
  // conflicts.
  const Rigid driverSpace = HeadInDriverSpace(in);
  const bool wfdIsIdentity = IsIdentity(Compose(H, Inverse(driverSpace)), 1e-6);
  if (!wfdIsIdentity && Length(got.t - driverSpace.t) <= kCheckPosTolM &&
      AngleBetween(got.q, driverSpace.q) <= kCheckRotTolRad) {
    g_rawPoseSemantics.store(2, std::memory_order_relaxed);
    g_conflictDownstream.store(false, std::memory_order_relaxed);
    return;
  }
  if (g_mismatchStreak.fetch_add(1, std::memory_order_relaxed) + 1 >= kCheckPersist)
    g_conflictDownstream.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The hook (hot path: lock-free, allocation-free, no logging)

void PoseHook(vr::IVRServerDriverHost* self, uint32_t id, const vr::DriverPose_t& in,
              uint32_t size) {
  const PoseFn prev = g_prev.load(std::memory_order_acquire);
  if (size != sizeof(vr::DriverPose_t) || g_disabled.load(std::memory_order_relaxed))
    return prev(self, id, in, size);

  const bool isHmd = id == g_hmdIndex.load(std::memory_order_relaxed);
  if (isHmd) PublishHmd(in);

  RSnapshot R;
  if (!ReadLocalR(&R) || R.identity) {
    prev(self, id, in, size);
    if (isHmd) MaybeCheckDownstream(self, id, in, RSnapshot{1, Rigid{}});
    return;
  }

  vr::DriverPose_t p = in;
  ApplyWorldTransform(R.r, &p);
  prev(self, id, p, size);
  if (isHmd) MaybeCheckDownstream(self, id, in, R);
}

// ---------------------------------------------------------------------------
// Direct-mode layers (layer_hook.h): take R back out of each layer's render
// pose, once the command thread has seen that the driver's render poses are in
// our space (g_layersValid), unless the submitLayerFixSkip setting names the
// driver.

// The hooks are for PC headsets (Steam Link and the like); the Frame presents
// its own frames, so Frame builds never install them.
#ifdef RECLINE_FRAME
constexpr bool kLayerHooks = false;
#else
constexpr bool kLayerHooks = true;
#endif

std::atomic<DeviceAddedFn> g_prevAdded{nullptr};
std::atomic<SubmitLayerFn> g_prevSubmit{nullptr};
// Display devices (HMDs, display redirects) added since the command thread last
// looked; a full queue drops the device (SteamVR adds one or two).
constexpr int kMaxAddedDisplays = 4;
std::atomic<vr::ITrackedDeviceServerDriver*> g_addedDisplays[kMaxAddedDisplays] = {};
std::atomic<bool> g_layerFix{true};
std::atomic<bool> g_layersValid{false};
std::atomic<uint64_t> g_layerCalls{0};

// A layer's render pose, sampled about once a second for the command thread.

struct alignas(64) LayerSample {
  uint32_t seq;
  double q[4];
  double t[3];
  uint64_t calls;
};
LayerSample g_layerSample{};
std::atomic<double> g_nextLayerSample{0.0};

void SampleLayer(const LayerPerEye& e, uint64_t calls) {
  const double now = MonotonicSeconds();
  double next = g_nextLayerSample.load(std::memory_order_relaxed);
  if (now < next) return;
  if (!g_nextLayerSample.compare_exchange_strong(next, now + 1.0, std::memory_order_relaxed)) return;
  const Rigid m = FromMatrix34(e.mHmdPose);
  LayerSample& b = g_layerSample;
  uint32_t s;
  if (!SeqTryLock(b.seq, &s)) return;
  St(b.q[0], m.q.w); St(b.q[1], m.q.x); St(b.q[2], m.q.y); St(b.q[3], m.q.z);
  St(b.t[0], m.t.x); St(b.t[1], m.t.y); St(b.t[2], m.t.z);
  St(b.calls, calls);
  SeqWriteEnd(b.seq, s);
}

void SubmitLayerHook(vr::IVRDriverDirectModeComponent* self, const LayerPerEye (&perEye)[2]) {
  const SubmitLayerFn prev = g_prevSubmit.load(std::memory_order_acquire);
  const uint64_t calls = g_layerCalls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (g_disabled.load(std::memory_order_relaxed)) return prev(self, perEye);
  SampleLayer(perEye[0], calls);  // as submitted, before any correction
  RSnapshot R;
  if (!g_layerFix.load(std::memory_order_relaxed) || !g_layersValid.load(std::memory_order_relaxed) ||
      !ReadLocalR(&R) || R.identity)
    return prev(self, perEye);
  // The layer was rendered from R ∘ H; tell the driver H, the pose its headset
  // knows, so its re-projection keeps R.
  LayerPerEye fixed[2] = {perEye[0], perEye[1]};
  const Rigid inv = Inverse(R.r);
  for (LayerPerEye& e : fixed) ToMatrix34(Compose(inv, FromMatrix34(e.mHmdPose)), &e.mHmdPose);
  prev(self, fixed);
}

bool DeviceAddedHook(vr::IVRServerDriverHost* self, const char* serial, vr::ETrackedDeviceClass cls,
                     vr::ITrackedDeviceServerDriver* drv) {
  const DeviceAddedFn prev = g_prevAdded.load(std::memory_order_acquire);
  const bool ok = prev(self, serial, cls, drv);
  // The frames go to the HMD, or to a display redirect that takes over its
  // display (SteamVR's PRISM compositor backend).
  if (ok && drv && !g_disabled.load() &&
      (cls == vr::TrackedDeviceClass_HMD || cls == vr::TrackedDeviceClass_DisplayRedirect)) {
    const char* what = cls == vr::TrackedDeviceClass_HMD ? "HMD" : "display redirect";
    bool queued = false;
    for (auto& slot : g_addedDisplays) {
      vr::ITrackedDeviceServerDriver* empty = nullptr;
      if (slot.compare_exchange_strong(empty, drv, std::memory_order_release)) {
        queued = true;
        break;
      }
    }
    Log("%s added: %s%s", what, serial ? serial : "?", queued ? "" : " (not checked: queue full)");
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Hook installation

bool InstallHook(vr::IVRServerDriverHost* host, std::string* why) {
  std::string info;
  const bool ok = InstallPoseHook(host, &PoseHook, &g_prev, why, &info);
  if (!info.empty()) Log("%s", info.c_str());
  return ok;
}

// ---------------------------------------------------------------------------
// Pin this module: unloading it must never unmap code that may still be in
// someone's call chain (e.g. a hook installed after ours that calls into us).

void PinSelf() {
  const void* self = reinterpret_cast<const void*>(&PoseHook);
  std::string err;
  if (!PinModuleOf(self, &err))
    Log("warning: could not pin module: %s", err.c_str());
  else
    Log("pinned %s", ModulePathOf(self).c_str());
}

// What clients compare with themselves before they act on our state
// (docs/DESIGN.md, "Driver identity").
DriverIdentity OwnIdentity() {
  DriverIdentity id;
  id.layout = kVersion;
  id.pid = uint32_t(ProcessId());
  id.version = RECLINE_VERSION;
  id.module_path = ModulePathOf(reinterpret_cast<const void*>(&PoseHook));
  if (!FileStamp(id.module_path, &id.module_size, &id.module_mtime))
    Log("warning: could not read %s", id.module_path.c_str());
  return id;
}

#ifdef RECLINE_FRAME
// Absolute path to the XRService shim, which ships next to this driver .so
// (.../bin/linuxarm64/librecline_xrshim.so). Empty if undeterminable.
std::string ShimPath() {
  std::string p = ModulePathOf(reinterpret_cast<const void*>(&PoseHook));
  const size_t slash = p.rfind('/');
  if (slash == std::string::npos) return {};
  return p.substr(0, slash + 1) + "librecline_xrshim.so";
}
#endif

// ---------------------------------------------------------------------------
// Driver provider

class ReclineProvider : public vr::IServerTrackedDeviceProvider {
 public:
  vr::EVRInitError Init(vr::IVRDriverContext* ctx) override {
    VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
    Log("driver_recline %s init (shm v%u)", RECLINE_VERSION, kVersion);
    PinSelf();
#ifdef RECLINE_FRAME
    shimPath_ = ShimPath();
#endif

    vr::EVRSettingsError se = vr::VRSettingsError_None;
    debugPitchDeg_ = vr::VRSettings()->GetFloat(kSection, "debugPitchDeg", &se);
    if (se != vr::VRSettingsError_None) debugPitchDeg_ = 0;
    se = vr::VRSettingsError_None;
    bool ds = vr::VRSettings()->GetBool(kSection, "downstreamCheck", &se);
    g_downstreamCheckEnabled.store(se == vr::VRSettingsError_None ? ds : true);
    if (debugPitchDeg_ != 0) Log("debugPitchDeg=%.2f", debugPitchDeg_);

    std::string err;
    ReclineShm* shm = OpenShm(/*driver=*/true, &err);
    if (!shm) Log("error: shared memory unavailable (%s); commands disabled", err.c_str());
    if (shm) {
      WriteIdentity(shm->identity, OwnIdentity());
      // Ignore commands written before this driver instance started: no
      // transform survives a restart, and the segment outlives vrserver.
      CmdSnapshot c;
      if (ReadCmd(shm->cmd, &c)) lastCmdId_ = c.cmd_id;
      StateSnapshot ss;
      ReadState(shm->state, &ss);  // keep cmd_ack and auto_reset monotonic for clients
      state_.cmd_ack = ss.cmd_ack;
      state_.auto_reset = ss.auto_reset & ~0xffu;
      // A fresh driver start resets the HMD liveness block's seq if a previous
      // vrserver died holding it (hook writers are in-process only).
      uint32_t hs = Ld(shm->hmd.seq);
      if (hs & 1u) St(shm->hmd.seq, hs + 1, std::memory_order_release);
    }
    WriteLocalR(Rigid{});
    state_.hmd_index = g_hmdIndex.load();

    host_ = vr::VRServerDriverHost();
    std::string why;
    if (!host_) {
      Log("error: no IVRServerDriverHost; not hooking");
      state_.flags |= FLAG_HOOK_REFUSED;
    } else if (!InstallHook(host_, &why)) {
      Log("hook refused: %s", why.c_str());
      state_.flags |= FLAG_HOOK_REFUSED;
    } else {
      g_hookInstalled.store(true);
      state_.flags |= FLAG_HOOK_INSTALLED;
      Log("hook installed; chained to %p", reinterpret_cast<void*>(g_prev.load()));
    }
    if (kLayerHooks && host_) {
      std::string info;
      why.clear();
      if (InstallDeviceAddedHook(host_, &DeviceAddedHook, &g_prevAdded, &why, &info))
        Log("TrackedDeviceAdded hook installed (%s)", info.c_str());
      else
        Log("TrackedDeviceAdded hook refused: %s%s%s", why.c_str(), info.empty() ? "" : "; ",
            info.c_str());
    }

    if (shm) {
      WriteState(shm->state, state_);
      g_shm.store(shm, std::memory_order_release);
    }
    shm_ = shm;
    running_.store(true);
    thread_ = std::thread([this] { CommandThread(); });
    return vr::VRInitError_None;
  }

  void Cleanup() override {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    // Pure pass-through from now on, whether or not we can unhook: wrappers
    // installed after us may still call into PoseHook.
    g_disabled.store(true, std::memory_order_release);
    if (g_hookInstalled.load()) {
      std::string msg;
      RemovePoseHook(&msg);
      Log("%s", msg.c_str());
    }
    if (shm_) {
      state_.flags &= ~(FLAG_TRANSFORM_ACTIVE | FLAG_HOOK_INSTALLED);
      state_.R_q[0] = 1; state_.R_q[1] = state_.R_q[2] = state_.R_q[3] = 0;
      state_.R_t[0] = state_.R_t[1] = state_.R_t[2] = 0;
      WriteState(shm_->state, state_);
    }
    // The shm mapping is intentionally left mapped: hook threads may still
    // touch it, and the module is pinned anyway.
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
  }

  const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }

  void RunFrame() override {
    vr::VREvent_t ev;
    bool resolve = false;
    const double now = MonotonicSeconds();
    while (host_ && host_->PollNextEvent(&ev, sizeof ev)) {
      if (ev.eventType == vr::VREvent_TrackedDeviceActivated ||
          ev.eventType == vr::VREvent_TrackedDeviceDeactivated ||
          ev.eventType == vr::VREvent_TrackedDeviceUpdated)
        resolve = true;
      // The headset's proximity sensor, for the automatic reset.
      if (ev.trackedDeviceIndex == g_hmdIndex.load(std::memory_order_relaxed)) {
        if (ev.eventType == vr::VREvent_TrackedDeviceUserInteractionEnded) {
          if (removedSince_.load() < 0) removedSince_.store(now);
        } else if (ev.eventType == vr::VREvent_TrackedDeviceUserInteractionStarted) {
          removedSince_.store(-1);
        }
      }
    }
    if (resolve || (!hmdVerified_ && now >= nextResolve_)) {
      nextResolve_ = now + 2.0;
      ResolveHmdIndex();
    }
  }

  bool ShouldBlockStandbyMode() override { return false; }
  void EnterStandby() override { standby_.store(true); }
  void LeaveStandby() override { standby_.store(false); }

 private:
  void ResolveHmdIndex() {
    auto* props = vr::VRProperties();
    if (!props) return;
    for (uint32_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
      const auto c = props->TrackedDeviceToPropertyContainer(i);
      if (c == vr::k_ulInvalidPropertyContainer) continue;
      vr::ETrackedPropertyError e = vr::TrackedProp_Success;
      const int32_t cls = props->GetInt32Property(c, vr::Prop_DeviceClass_Int32, &e);
      if (e == vr::TrackedProp_Success && cls == vr::TrackedDeviceClass_HMD) {
        if (!hmdVerified_ || g_hmdIndex.load() != i)
          Log("HMD is tracked device %u", i);
        g_hmdIndex.store(i, std::memory_order_relaxed);
        hmdVerified_ = true;
        return;
      }
    }
  }

  // Install the targeted XRService spawn injection as soon as
  // libArcturusPerception (driver_cv's dependency, which spawns XRService) is
  // loaded. It isn't present yet at our Init, so the command thread polls for
  // it; the GOT hook then persists across XRService restarts.
  void MaybeInjectSpawn() {
#ifdef RECLINE_FRAME
    if (spawnInjectDone_ || shimPath_.empty()) return;
    if (!ModuleLoaded("libArcturusPerception.so")) return;
    const int n = InstallSpawnInjection(shimPath_, [](const std::string& m) { Log("%s", m.c_str()); });
    spawnInjectDone_ = true;  // one attempt once the module is present
    Log("spawn injection: %d/3 spawn functions hooked (shim %s)", n, shimPath_.c_str());
#endif
  }

  void CommandThread() {
    double nextSlow = 0, staleSince = 0;
    uint32_t staleSeq = 0;
    bool debugApplied = debugPitchDeg_ == 0;
    uint32_t lastLoggedFlags = state_.flags, lastLoggedSemantics = 0;
    while (running_.load(std::memory_order_relaxed)) {
      const double now = MonotonicSeconds();
      bool stateDirty = false;

      MaybeInjectSpawn();
      MaybeHookLayers(now);
      CheckLayerSample();

      if (shm_) {
        // Crashed-writer recovery: release a seq stuck at the same odd value
        // for > 1 s; the (possibly torn) payload is then validated like any
        // other command.
        const uint32_t s = Ld(shm_->cmd.seq);
        if (s & 1u) {
          if (s != staleSeq) {
            staleSeq = s;
            staleSince = now;
          } else if (now - staleSince > 1.0) {
            uint32_t exp = s;
            if (Atomic(shm_->cmd.seq).compare_exchange_strong(exp, s + 1, std::memory_order_release,
                                                              std::memory_order_relaxed))
              Log("command block was left locked by a crashed writer; released");
            staleSeq = 0;
          }
        } else {
          staleSeq = 0;
        }

        CmdSnapshot c;
        if (ReadCmd(shm_->cmd, &c, 64) && c.cmd_id != lastCmdId_) {
          lastCmdId_ = c.cmd_id;
          ApplyCommand(c);
          stateDirty = true;
        }
      }

      if (!debugApplied) debugApplied = TryApplyDebugPitch(&stateDirty);

      if (guard_.Armed()) {
        Rigid H;
        const bool have = LatestHead(&H);
        const uint32_t why = guard_.Update(now, have, H.t, removedSince_.load(), standby_.load());
        if (why != AUTO_RESET_NONE) {
          AutoReset(why);
          stateDirty = true;
        }
      }

      if (now >= nextSlow) {
        nextSlow = now + 1.0;
        ReadLayerFixSetting();
        uint32_t flags = state_.flags;
        // wrapped: someone replaced our slot entry, so their transform is applied
        // before R. Expected (e.g. a calibrator), but reported.
        if (g_hookInstalled.load()) {
          if (PoseHookWrapped()) flags |= FLAG_WRAPPED;
          else flags &= ~FLAG_WRAPPED;
        }
        if (layersHooked_ && g_layerFix.load() && g_layersValid.load()) flags |= FLAG_LAYERS_CORRECTED;
        else flags &= ~FLAG_LAYERS_CORRECTED;
        if (g_conflictDownstream.load(std::memory_order_relaxed)) flags |= FLAG_CONFLICT_DOWNSTREAM;
        else flags &= ~FLAG_CONFLICT_DOWNSTREAM;
        if (state_.hmd_index != g_hmdIndex.load()) {
          state_.hmd_index = g_hmdIndex.load();
          stateDirty = true;
        }
        if (flags != state_.flags) {
          state_.flags = flags;
          stateDirty = true;
        }
        if (flags != lastLoggedFlags) {
          if ((flags ^ lastLoggedFlags) & FLAG_WRAPPED)
            Log((flags & FLAG_WRAPPED)
                    ? "another pose hook wraps ours (its transform applies before R)"
                    : "no longer wrapped: our hook is outermost again");
          if ((flags ^ lastLoggedFlags) & FLAG_CONFLICT_DOWNSTREAM)
            Log((flags & FLAG_CONFLICT_DOWNSTREAM)
                    ? "conflict: HMD poses are modified after our hook (last err %.1f mm / %.2f deg)"
                    : "downstream conflict cleared",
                g_lastCheckPosErr.load() * 1000.0, Deg(g_lastCheckRotErr.load()));
          lastLoggedFlags = flags;
        }
        const int sem = g_rawPoseSemantics.load();
        if (uint32_t(sem) != lastLoggedSemantics) {
          Log(sem == 1 ? "GetRawTrackedDevicePoses returns post-world-from-driver poses; "
                         "downstream check active (err %.2f mm / %.3f deg)"
                       : "GetRawTrackedDevicePoses returns driver-space poses; downstream "
                         "check disabled (err %.2f mm / %.3f deg)",
              g_lastCheckPosErr.load() * 1000.0, Deg(g_lastCheckRotErr.load()));
          lastLoggedSemantics = uint32_t(sem);
        }
      }

      if (stateDirty && shm_) WriteState(shm_->state, state_);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));  // ~100 Hz
    }
  }

  // A display device's direct-mode component, once its driver has added it
  // (it may only provide the component once activated: retry for a while).
  struct PendingDisplay {
    vr::ITrackedDeviceServerDriver* drv = nullptr;
    int tries = 0;
    double next = 0;
  };

  void MaybeHookLayers(double now) {
    for (auto& slot : g_addedDisplays) {
      vr::ITrackedDeviceServerDriver* drv = slot.exchange(nullptr, std::memory_order_acquire);
      if (!drv) continue;
      for (PendingDisplay& p : pendingDisplays_) {
        if (!p.drv) {
          p = {drv, 0, now};
          break;
        }
      }
    }
    for (PendingDisplay& p : pendingDisplays_) {
      if (!p.drv || now < p.next) continue;
      auto* comp = static_cast<vr::IVRDriverDirectModeComponent*>(
          p.drv->GetComponent(vr::IVRDriverDirectModeComponent_Version));
      if (!comp && ++p.tries < 10) {
        p.next = now + 1.0;
        continue;
      }
      p = {};
      if (comp) HookLayers(comp);
      else Log("a display device has no %s (frames not corrected)", vr::IVRDriverDirectModeComponent_Version);
    }
  }

  void HookLayers(vr::IVRDriverDirectModeComponent* comp) {
    std::string why, info;
    std::string module;
    const bool ok = InstallSubmitLayerHook(comp, &SubmitLayerHook, &g_prevSubmit, &module, &why, &info);
    if (ok)
      Log("SubmitLayer hook installed (%s)", info.empty() ? "already" : info.c_str());
    else
      Log("SubmitLayer hook refused: %s%s%s", why.c_str(), info.empty() ? "" : "; ", info.c_str());
    if (ok) {
      if (!module.empty()) layerDriver_ = DriverNameFromModule(module);
      layersHooked_ = true;
      g_layersValid.store(false);  // a new display device: check its poses afresh
      ResetLayerCheck();
      ReadLayerFixSetting(/*announce=*/true);
    }
  }

  // submitLayerFixSkip: drivers whose frames are not to be corrected, by name
  // (see DriverNameFromModule), or "*".
  void ReadLayerFixSetting(bool announce = false) {
    char list[256] = {};
    vr::EVRSettingsError se = vr::VRSettingsError_None;
    vr::VRSettings()->GetString(kSection, "submitLayerFixSkip", list, sizeof list, &se);
    const bool fix = se != vr::VRSettingsError_None || !ListNames(list, layerDriver_);
    if (fix == g_layerFix.load() && !announce) return;
    g_layerFix.store(fix);
    if (layersHooked_)
      Log("frames from driver '%s' %s", layerDriver_.c_str(),
          fix ? "are corrected" : "are not corrected (submitLayerFixSkip)");
  }

  // Runtime check that the render poses vrserver hands the direct-mode driver
  // are what this correction assumes: our head pose with R applied (R ∘ H),
  // give or take the driver's prediction. vrserver, not the driver, fills them
  // in, so this guards against SteamVR changing their space (e.g. to driver
  // space, before world-from-driver), not against differences between
  // drivers. Each new sample is compared as it arrives (it is at most one loop
  // old); the last kLayerWindow samples decide, with hysteresis.
  static constexpr int kLayerWindow = 10, kLayerValidAt = 8, kLayerInvalidAt = 4;
  static constexpr double kLayerTolRad = 10.0 * kPi / 180.0, kLayerTolM = 0.10;
  static constexpr int kLayerMaxLogs = 10;

  void ResetLayerCheck() {
    layerWindow_ = 0;
    layerSamples_ = 0;
  }

  void CheckLayerSample() {
    if (!layersHooked_) return;
    LayerSample s;
    const LayerSample& b = g_layerSample;
    if (!SeqRead(b.seq, [&] {
          for (int i = 0; i < 4; ++i) s.q[i] = Ld(b.q[i]);
          for (int i = 0; i < 3; ++i) s.t[i] = Ld(b.t[i]);
          s.calls = Ld(b.calls);
        }, 64))
      return;
    if (s.calls == layerCheckedCalls_) return;
    layerCheckedCalls_ = s.calls;
    Rigid H;
    RSnapshot R;
    if (!LatestHead(&H) || !ReadLocalR(&R)) return;
    const Rigid M{{s.q[0], s.q[1], s.q[2], s.q[3]}, {s.t[0], s.t[1], s.t[2]}};
    const Rigid RH = R.identity ? H : Compose(R.r, H);
    const double rotErr = AngleBetween(Normalize(M.q), RH.q), posErr = Length(M.t - RH.t);
    const bool match = rotErr <= kLayerTolRad && posErr <= kLayerTolM;
    layerWindow_ = ((layerWindow_ << 1) | (match ? 1u : 0u)) & ((1u << kLayerWindow) - 1);
    if (layerSamples_ < kLayerWindow) ++layerSamples_;
    int matches = 0;
    for (uint32_t w = layerWindow_; w; w &= w - 1) ++matches;
    const bool valid = g_layersValid.load();
    bool next = valid;
    if (!valid && matches >= kLayerValidAt) next = true;
    if (valid && layerSamples_ == kLayerWindow && matches <= kLayerInvalidAt) next = false;
    if (next == valid) return;
    g_layersValid.store(next);
    if (layerLogs_ < kLayerMaxLogs) {
      ++layerLogs_;
      Log(next ? "direct-mode render poses match ours (last %.1f deg, %.0f mm): correcting them"
               : "direct-mode render poses no longer match ours (last %.1f deg, %.0f mm): not correcting",
          Deg(rotErr), posErr * 1000);
    }
  }

  bool LatestHead(Rigid* h) {
    if (!shm_) return false;
    HmdSnapshot s;
    if (!ReadHmd(shm_->hmd, &s, 1000)) return false;
    if (s.time == 0 || MonotonicSeconds() - s.time > 0.5) return false;
    Rigid r{{s.q[0], s.q[1], s.q[2], s.q[3]}, {s.t[0], s.t[1], s.t[2]}};
    if (!IsSaneRigid(r, 1e4)) return false;
    r.q = Normalize(r.q);
    *h = r;
    return true;
  }

  void SetR(const Rigid& r) {
    WriteLocalR(r);  // in-process copy first, then StateBlock
    state_.R_q[0] = r.q.w; state_.R_q[1] = r.q.x; state_.R_q[2] = r.q.y; state_.R_q[3] = r.q.z;
    state_.R_t[0] = r.t.x; state_.R_t[1] = r.t.y; state_.R_t[2] = r.t.z;
    if (IsIdentity(r)) state_.flags &= ~FLAG_TRANSFORM_ACTIVE;
    else state_.flags |= FLAG_TRANSFORM_ACTIVE;
    // Watch the real head from where it is now; without a head pose there is
    // nothing to measure movement against.
    Rigid H;
    if (!IsIdentity(r) && LatestHead(&H)) guard_.Arm(H.t);
    else guard_.Disarm();
    // A new R changes what "expected" means for the downstream check.
    g_mismatchStreak.store(0, std::memory_order_relaxed);
    // Samples taken under the old R compare against the wrong pose.
    ResetLayerCheck();
  }

  // The user moved away or took the headset off while reclined: undo R so
  // SteamVR's boundary and passthrough are right again.
  void AutoReset(uint32_t why) {
    SetR(Rigid{});
    state_.auto_reset = (((state_.auto_reset >> 8) + 1) << 8) | why;
    Log("automatic reset: %s", why == AUTO_RESET_MOVED ? "the head moved away" : "headset taken off");
  }

  void LogR(const char* what, const Rigid& r) {
    double y, p, ro;
    ToYawPitchRoll(r.q, &y, &p, &ro);
    Log("%s: R yaw %.1f pitch %.1f roll %.1f deg, t (%.3f, %.3f, %.3f) m", what, Deg(y), Deg(p),
        Deg(ro), r.t.x, r.t.y, r.t.z);
  }

  void ApplyCommand(const CmdSnapshot& c) {
    const Rigid payload{{c.q[0], c.q[1], c.q[2], c.q[3]}, {c.t[0], c.t[1], c.t[2]}};
    uint32_t result = RES_OK;
    switch (c.cmd) {
      case CMD_RECENTER: {
        if (!IsSaneRigid(payload)) {
          result = RES_INVALID_ARGS;
          break;
        }
        Rigid H;
        if (!LatestHead(&H)) {
          result = RES_NO_HMD_POSE;
          break;
        }
        // R = Traw ∘ H⁻¹, so that R ∘ H = Traw now.
        Rigid Traw = payload;
        Traw.q = Normalize(Traw.q);
        Rigid R = Compose(Traw, Inverse(H));
        R.q = Normalize(R.q);
        SetR(R);
        LogR("recenter", R);
        break;
      }
      case CMD_RESET:
        SetR(Rigid{});
        Log("reset: R = identity");
        break;
      case CMD_SET: {
        if (!IsSaneRigid(payload)) {
          result = RES_INVALID_ARGS;
          break;
        }
        Rigid R = payload;
        R.q = Normalize(R.q);
        SetR(R);
        LogR("set", R);
        break;
      }
      default:
        result = RES_INVALID_ARGS;
        break;
    }
    if (result != RES_OK) Log("command %u (id %u) rejected: result %u", c.cmd, c.cmd_id, result);
    state_.cmd_ack = c.cmd_id;
    state_.cmd_result = result;
  }

  // debugPitchDeg: fixed pitch about the head position, applied once the first
  // HMD pose arrives (for testing without any client).
  bool TryApplyDebugPitch(bool* dirty) {
    Rigid H;
    if (!LatestHead(&H)) {
      // Without shm there's no published H; pitch about the origin instead
      // after giving the HMD a moment to appear.
      if (shm_ || g_hmdPoseCount.load() == 0) return false;
      H = Rigid{};
    }
    Rigid R;
    R.q = AxisAngle({1, 0, 0}, Rad(debugPitchDeg_));
    R.t = H.t - Rotate(R.q, H.t);  // rotate about the head position
    SetR(R);
    LogR("debugPitchDeg applied", R);
    *dirty = true;
    return true;
  }

  vr::IVRServerDriverHost* host_ = nullptr;
  ReclineShm* shm_ = nullptr;
  StateSnapshot state_;
  uint32_t lastCmdId_ = 0;
  double debugPitchDeg_ = 0;
#ifdef RECLINE_FRAME
  std::string shimPath_;
  bool spawnInjectDone_ = false;
#endif
  bool hmdVerified_ = false;
  PendingDisplay pendingDisplays_[kMaxAddedDisplays];  // command thread only, from here
  bool layersHooked_ = false;
  std::string layerDriver_;  // the hooked SubmitLayer's driver, e.g. "vrlink"
  uint32_t layerWindow_ = 0;  // bit i: sample i back matched
  int layerSamples_ = 0;
  int layerLogs_ = 0;
  uint64_t layerCheckedCalls_ = 0;
  double nextResolve_ = 0;
  AutoResetGuard guard_;                  // command thread only
  std::atomic<double> removedSince_{-1};  // RunFrame -> command thread
  std::atomic<bool> standby_{false};
  std::atomic<bool> running_{false};
  std::thread thread_;
};

ReclineProvider g_provider;

}  // namespace

RECLINE_EXPORT void* HmdDriverFactory(const char* pInterfaceName, int* pReturnCode) {
  if (strcmp(pInterfaceName, vr::IServerTrackedDeviceProvider_Version) == 0)
    return static_cast<vr::IServerTrackedDeviceProvider*>(&g_provider);
  if (pReturnCode) *pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
  return nullptr;
}
