// The pose hook (driver/pose_hook.h) on a fake IVRServerDriverHost: install,
// chaining, removal, and ordering against a Space Calibrator-style hook
// installed after ours, which must apply its transform first (R ∘ Cal ∘ W).
#include <cstdio>
#include <string>

#include "fake_calibrator.h"
#include "pose_hook.h"

using namespace recline;

namespace {

int g_failures = 0;
#define CHECK(cond, ...)                                     \
  do {                                                       \
    if (!(cond)) {                                           \
      ++g_failures;                                          \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
      fprintf(stderr, __VA_ARGS__);                          \
      fprintf(stderr, "\n");                                 \
    }                                                        \
  } while (0)

// Each stage appends its digit to vecPosition[0], so the value the host
// receives spells the order in which the stages ran.
int g_received = -1;
int g_lastHost = 0;  // also keeps the FakeHost bodies distinct (no code folding)

void Stage(vr::DriverPose_t* p, int digit) { p->vecPosition[0] = p->vecPosition[0] * 10 + digit; }

// Two distinct classes, so each scenario hooks a function of its own.
template <int N>
class FakeHost : public vr::IVRServerDriverHost {
 public:
  bool TrackedDeviceAdded(const char*, vr::ETrackedDeviceClass, vr::ITrackedDeviceServerDriver*) override {
    return false;
  }
  void TrackedDevicePoseUpdated(uint32_t, const vr::DriverPose_t& p, uint32_t) override {
    g_received = int(p.vecPosition[0]);
    g_lastHost = N;
  }
  void VsyncEvent(double) override {}
  void VendorSpecificEvent(uint32_t, vr::EVREventType, const vr::VREvent_Data_t&, double) override {}
  bool IsExiting() override { return false; }
  bool PollNextEvent(vr::VREvent_t*, uint32_t) override { return false; }
  void GetRawTrackedDevicePoses(float, vr::TrackedDevicePose_t*, uint32_t) override {}
  void RequestRestart(const char*, const char*, const char*, const char*) override {}
  uint32_t GetFrameTimings(vr::Compositor_FrameTiming*, uint32_t) override { return 0; }
  void SetDisplayEyeToHead(uint32_t, const vr::HmdMatrix34_t&, const vr::HmdMatrix34_t&) override {}
  void SetDisplayProjectionRaw(uint32_t, const vr::HmdRect2_t&, const vr::HmdRect2_t&) override {}
  void SetRecommendedRenderTargetSize(uint32_t, uint32_t, uint32_t) override {}
};

std::atomic<PoseFn> g_prev{nullptr};
PoseFn g_calibratorPrev = nullptr;

void ReclineHook(vr::IVRServerDriverHost* self, uint32_t id, const vr::DriverPose_t& in,
                 uint32_t size) {
  vr::DriverPose_t p = in;
  Stage(&p, 1);
  g_prev.load()(self, id, p, size);
}

void CalibratorHook(vr::IVRServerDriverHost* self, uint32_t id, const vr::DriverPose_t& in,
                    uint32_t size) {
  vr::DriverPose_t p = in;
  Stage(&p, 2);
  g_calibratorPrev(self, id, p, size);
}

// Through a volatile pointer, so the call can't be devirtualized.
int Send(vr::IVRServerDriverHost* h) {
  vr::IVRServerDriverHost* volatile host = h;
  vr::DriverPose_t p{};
  g_received = -1;
  host->TrackedDevicePoseUpdated(0, p, sizeof p);
  return g_received;
}

void TestInstallRemove() {
  FakeHost<1> real;
  vr::IVRServerDriverHost* host = &real;
  std::string why, info, msg;
  CHECK(Send(host) == 0, "unhooked: %d", Send(host));
  const bool ok = InstallPoseHook(host, &ReclineHook, &g_prev, &why, &info);
  printf("install: %s\n", ok ? info.c_str() : why.c_str());
  CHECK(ok, "install: %s", why.c_str());
  CHECK(Send(host) == 1, "hooked: %d", Send(host));
  CHECK(!PoseHookWrapped(), "not wrapped yet");
  CHECK(RemovePoseHook(&msg), "remove: %s", msg.c_str());
  CHECK(Send(host) == 0, "removed: %d", Send(host));
}

void TestCalibratorOrder() {
  FakeHost<2> real;
  vr::IVRServerDriverHost* host = &real;
  std::string why, info, msg;
  CHECK(InstallPoseHook(host, &ReclineHook, &g_prev, &why, &info), "install: %s", why.c_str());
  CHECK(Send(host) == 1, "hooked: %d", Send(host));
  CHECK(InstallFakeCalibrator(host, &CalibratorHook, &g_calibratorPrev, &why), "calibrator: %s",
        why.c_str());
  // Calibrator (2) first, then recline (1): 21. The reverse order would be 12.
  const int got = Send(host);
  printf("calibrator, then recline: %d\n", got);
  CHECK(got == 21, "order: got %d, want 21", got);
  CHECK(PoseHookWrapped(), "wrapped by the calibrator");
  CHECK(!RemovePoseHook(&msg), "remove must refuse while wrapped");
  printf("remove while wrapped: %s\n", msg.c_str());
  CHECK(Send(host) == 21, "still chained: %d", Send(host));
}

}  // namespace

int main() {
  TestInstallRemove();
  TestCalibratorOrder();
  if (g_failures) fprintf(stderr, "%d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
