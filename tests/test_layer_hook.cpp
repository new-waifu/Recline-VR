// The layer hooks (driver/layer_hook.h) on a fake IVRServerDriverHost and fake
// direct-mode components: install, chaining, the same driver hooked twice, a
// second direct-mode driver refused; and the submitLayerFixSkip helpers.
#include <cstdio>
#include <string>

#include "layer_hook.h"

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

int g_addedClass = -1;
double g_receivedX = -1;
int g_lastComponent = 0;  // also keeps the FakeComponent bodies distinct (no code folding)

class FakeHost : public vr::IVRServerDriverHost {
 public:
  bool TrackedDeviceAdded(const char*, vr::ETrackedDeviceClass cls, vr::ITrackedDeviceServerDriver*) override {
    g_addedClass = int(cls);
    return true;
  }
  void TrackedDevicePoseUpdated(uint32_t, const vr::DriverPose_t&, uint32_t) override {}
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

// Two distinct classes: two direct-mode drivers.
template <int N>
class FakeComponent : public vr::IVRDriverDirectModeComponent {
 public:
  void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override {
    g_receivedX = perEye[0].mHmdPose.m[0][3] + perEye[1].mHmdPose.m[0][3];
    g_lastComponent = N;
  }
};

std::atomic<DeviceAddedFn> g_prevAdded{nullptr};
std::atomic<SubmitLayerFn> g_prevSubmit{nullptr};
std::atomic<SubmitLayerFn> g_prevSubmit2{nullptr};
int g_hookSawClass = -1;

bool AddedHook(vr::IVRServerDriverHost* self, const char* serial, vr::ETrackedDeviceClass cls,
               vr::ITrackedDeviceServerDriver* drv) {
  g_hookSawClass = int(cls);
  return g_prevAdded.load()(self, serial, cls, drv);
}

// Moves each eye's pose 1 m along x, as the real hook rewrites mHmdPose.
void SubmitHook(vr::IVRDriverDirectModeComponent* self, const LayerPerEye (&perEye)[2]) {
  LayerPerEye fixed[2] = {perEye[0], perEye[1]};
  for (LayerPerEye& e : fixed) e.mHmdPose.m[0][3] += 1.0;
  g_prevSubmit.load()(self, fixed);
}

// Through volatile pointers, so the calls can't be devirtualized.
bool Add(vr::IVRServerDriverHost* h) {
  vr::IVRServerDriverHost* volatile host = h;
  return host->TrackedDeviceAdded("serial", vr::TrackedDeviceClass_HMD, nullptr);
}

double Submit(vr::IVRDriverDirectModeComponent* c) {
  vr::IVRDriverDirectModeComponent* volatile comp = c;
  LayerPerEye perEye[2]{};
  g_receivedX = -1;
  comp->SubmitLayer(perEye);
  return g_receivedX;
}

void TestDeviceAdded() {
  FakeHost real;
  std::string why, info;
  const bool ok = InstallDeviceAddedHook(&real, &AddedHook, &g_prevAdded, &why, &info);
  printf("device added: %s\n", ok ? info.c_str() : why.c_str());
  CHECK(ok, "install: %s", why.c_str());
  CHECK(Add(&real), "chained call returns the host's result");
  CHECK(g_hookSawClass == vr::TrackedDeviceClass_HMD, "hook saw class %d", g_hookSawClass);
  CHECK(g_addedClass == vr::TrackedDeviceClass_HMD, "host got class %d", g_addedClass);
}

void TestSubmitLayer() {
  FakeComponent<1> first;
  FakeComponent<2> second;
  std::string why, info, module;
  CHECK(Submit(&first) == 0, "unhooked: %g", Submit(&first));
  const bool ok = InstallSubmitLayerHook(&first, &SubmitHook, &g_prevSubmit, &module, &why, &info);
  printf("submit layer: %s\n", ok ? info.c_str() : why.c_str());
  CHECK(ok, "install: %s", why.c_str());
  // The fake component lives in this test binary.
  CHECK(module.find("test_layer_hook") != std::string::npos, "module: '%s'", module.c_str());
  CHECK(Submit(&first) == 2, "hooked: both eyes rewritten, got %g", Submit(&first));
  // The same driver adding its HMD again.
  FakeComponent<1> again;
  why.clear();
  CHECK(InstallSubmitLayerHook(&again, &SubmitHook, &g_prevSubmit, &module, &why, &info),
        "same driver: %s", why.c_str());
  CHECK(Submit(&again) == 2, "same driver still hooked once: %g", Submit(&again));
  // A second direct-mode driver is refused and left alone.
  why.clear();
  CHECK(!InstallSubmitLayerHook(&second, &SubmitHook, &g_prevSubmit2, &module, &why, &info),
        "second driver must be refused");
  printf("second driver: %s\n", why.c_str());
  CHECK(Submit(&second) == 0 && g_lastComponent == 2, "second driver untouched: %g", Submit(&second));
}

void TestSkipList() {
  CHECK(DriverNameFromModule("C:\\SteamVR\\drivers\\vrlink\\bin\\win64\\driver_vrlink.dll") == "vrlink",
        "windows path");
  CHECK(DriverNameFromModule("/opt/steamvr/drivers/vrlink/bin/linux64/driver_vrlink.so") == "vrlink",
        "linux path");
  CHECK(DriverNameFromModule("driver_ALVR_server.dll") == "alvr_server", "lower case");
  CHECK(DriverNameFromModule("/usr/bin/vrserver").empty(), "not a driver");
  CHECK(DriverNameFromModule("driver_.dll").empty(), "no name");
  CHECK(!ListNames("", "vrlink"), "empty list");
  CHECK(ListNames("vrlink", "vrlink"), "single");
  CHECK(ListNames("oculus, VRLink", "vrlink"), "case and spaces");
  CHECK(ListNames("oculus,prism", "prism"), "comma");
  CHECK(!ListNames("vrlinkx,oculus", "vrlink"), "no prefix match");
  CHECK(ListNames("*", "anything"), "star");
  CHECK(!ListNames("vrlink", ""), "unknown driver only by star");
  CHECK(ListNames("*", ""), "star covers unknown");
}

}  // namespace

int main() {
  TestDeviceAdded();
  TestSubmitLayer();
  TestSkipList();
  if (g_failures) fprintf(stderr, "%d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
