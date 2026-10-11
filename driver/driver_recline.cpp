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
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include "recline_math.h"
#include "pose_math.h"
#include "recline_shm.h"
#include "slot_patch.h"
#ifdef RECLINE_FRAME
#include "spawn_inject.h"
#endif

#define RECLINE_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

using namespace recline;

using PoseFn = void (*)(vr::IVRServerDriverHost*, uint32_t, const vr::DriverPose_t&, uint32_t);

constexpr const char* kSection = "driver_recline";

// ---------------------------------------------------------------------------
// Logging (never from the hot path)

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
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
void** g_slot = nullptr;  // &vtbl[index of TrackedDevicePoseUpdated]

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
// Hook installation

bool InstallHook(vr::IVRServerDriverHost* host, std::string* why) {
  void** slot;
  void* cur;
  std::string info;
  const size_t idx = VirtualSlotIndex(&vr::IVRServerDriverHost::TrackedDevicePoseUpdated);
  const bool located = LocateVtableSlot(host, idx, nullptr, &slot, &cur, why, &info);
  if (!info.empty()) Log("%s", info.c_str());
  if (!located) return false;
  if (cur == reinterpret_cast<void*>(&PoseHook)) {
    *why = "hook already installed (re-init)";
    return false;
  }
  // Store prev before the hook can run; chain to whatever was there (another
  // vtable hook or the original thunk).
  g_prev.store(reinterpret_cast<PoseFn>(cur), std::memory_order_release);
  std::string err;
  const bool ok = PatchSlot(slot, cur, reinterpret_cast<void*>(&PoseHook), &err);
  if (!err.empty() && ok) Log("%s", err.c_str());
  if (!ok) {
    *why = "patching vtable slot failed: " + err;
    return false;
  }
  g_slot = slot;
  return true;
}

// ---------------------------------------------------------------------------
// Pin this .so: a later dlclose must never unmap code that may still be in
// someone's call chain (e.g. a hook installed after ours that calls into us).

void PinSelf() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&PoseHook), &info) || !info.dli_fname) {
    Log("warning: dladdr failed, cannot pin module");
    return;
  }
  if (!dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE))
    Log("warning: RTLD_NODELETE pin of %s failed: %s", info.dli_fname, dlerror());
  else
    Log("pinned %s", info.dli_fname);
}

#ifdef RECLINE_FRAME
// Absolute path to the XRService shim, which ships next to this driver .so
// (.../recline/bin/linuxarm64/librecline_xrshim.so). Empty if undeterminable.
std::string ShimPath() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&PoseHook), &info) || !info.dli_fname) return {};
  std::string p = info.dli_fname;
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
    Log("driver_recline init (shm v%u)", kVersion);
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
    ReclineShm* shm = OpenShm(/*takeOverUninitialized=*/true, &err);
    if (!shm) Log("error: shared memory unavailable (%s); commands disabled", err.c_str());
    if (shm) {
      // Ignore commands written before this driver instance started: no
      // transform survives a restart, and the segment outlives vrserver.
      CmdSnapshot c;
      if (ReadCmd(shm->cmd, &c)) lastCmdId_ = c.cmd_id;
      StateSnapshot ss;
      ReadState(shm->state, &ss);  // keep cmd_ack monotonic for clients
      state_.cmd_ack = ss.cmd_ack;
      // A fresh driver start resets the HMD liveness block's seq if a previous
      // vrserver died holding it (hook writers are in-process only).
      uint32_t hs = __atomic_load_n(&shm->hmd.seq, __ATOMIC_RELAXED);
      if (hs & 1u) __atomic_store_n(&shm->hmd.seq, hs + 1, __ATOMIC_RELEASE);
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
    if (g_hookInstalled.load() && g_slot) {
      void* cur = __atomic_load_n(g_slot, __ATOMIC_ACQUIRE);
      if (cur == reinterpret_cast<void*>(&PoseHook)) {
        std::string err;
        if (PatchSlot(g_slot, cur, reinterpret_cast<void*>(g_prev.load()), &err))
          Log("hook removed%s%s", err.empty() ? "" : "; ", err.c_str());
        else
          Log("warning: could not remove hook (%s); staying as pass-through", err.c_str());
      } else {
        Log("hook was wrapped by %p; staying installed as pass-through", cur);
      }
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
    while (host_ && host_->PollNextEvent(&ev, sizeof ev)) {
      if (ev.eventType == vr::VREvent_TrackedDeviceActivated ||
          ev.eventType == vr::VREvent_TrackedDeviceDeactivated ||
          ev.eventType == vr::VREvent_TrackedDeviceUpdated)
        resolve = true;
    }
    const double now = MonotonicSeconds();
    if (resolve || (!hmdVerified_ && now >= nextResolve_)) {
      nextResolve_ = now + 2.0;
      ResolveHmdIndex();
    }
  }

  bool ShouldBlockStandbyMode() override { return false; }
  void EnterStandby() override {}
  void LeaveStandby() override {}

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

      if (shm_) {
        // Crashed-writer recovery: release a seq stuck at the same odd value
        // for > 1 s; the (possibly torn) payload is then validated like any
        // other command.
        const uint32_t s = __atomic_load_n(&shm_->cmd.seq, __ATOMIC_RELAXED);
        if (s & 1u) {
          if (s != staleSeq) {
            staleSeq = s;
            staleSince = now;
          } else if (now - staleSince > 1.0) {
            uint32_t exp = s;
            if (__atomic_compare_exchange_n(&shm_->cmd.seq, &exp, s + 1, false,
                                            __ATOMIC_RELEASE, __ATOMIC_RELAXED))
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

      if (now >= nextSlow) {
        nextSlow = now + 1.0;
        uint32_t flags = state_.flags;
        // wrapped: someone replaced our slot entry, so their transform is applied
        // before R. Expected (e.g. a calibrator), but reported.
        if (g_hookInstalled.load() && g_slot) {
          void* cur = __atomic_load_n(g_slot, __ATOMIC_RELAXED);
          if (cur != reinterpret_cast<void*>(&PoseHook)) flags |= FLAG_WRAPPED;
          else flags &= ~FLAG_WRAPPED;
        }
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
      usleep(10000);  // ~100 Hz
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
    // A new R changes what "expected" means for the downstream check.
    g_mismatchStreak.store(0, std::memory_order_relaxed);
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
  double nextResolve_ = 0;
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
