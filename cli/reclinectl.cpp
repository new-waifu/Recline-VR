// reclinectl: command-line client for driver_recline.
//
//   reclinectl status
//   reclinectl recenter [--keep-position] [--level-only] [--height M] [--no-seated-reset]
//     (undocumented: --head-axis levels the head pose's forward axis instead of the
//      lens-centre view axis; diagnostic only)
//   reclinectl reset [--no-seated-reset]
//   reclinectl set <yaw> <pitch> <roll> <x> <y> <z>     (degrees, meters)
//   reclinectl poses      (prints device poses per tracking universe, for testing)
//   reclinectl panel show|hide
//   reclinectl register | unregister   (the overlay, with SteamVR; used by the installers)

#include <openvr.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>


#include "overlay_options.h"
#include "platform.h"
#include "recline_client.h"
#include "recline_version.h"

using namespace recline;

namespace {

void Usage() {
  fprintf(stderr,
          "usage: reclinectl status | --version\n"
          "       reclinectl recenter [--keep-position] [--level-only] [--height M]\n"
          "                           [--no-seated-reset]\n"
          "       reclinectl reset [--no-seated-reset]\n"
          "       reclinectl poses        (HMD/controller poses as apps see them)\n"
          "       reclinectl props        (SteamVR drivers and device string properties)\n"
          "       reclinectl bindings     (open the controller binding editor in the headset)\n"
          "       reclinectl panel show|hide   (the overlay's icon in the SteamVR dashboard)\n"
          "       reclinectl register | unregister   (start the overlay with SteamVR, or stop)\n"
          "       reclinectl setting get <section> <key>\n"
          "       reclinectl setting set <section> <key> <bool|int|float|string>\n"
          "       reclinectl setting remove <section> <key>\n"
          "       reclinectl set <yaw> <pitch> <roll> <x> <y> <z>   (degrees, meters)\n");
}

// The driver, checked against this build as the overlay checks it (see
// Diagnose): reclinectl acts on the same terms.
struct Driver {
  ReclineShm* shm = nullptr;
  std::string openError;
  DriverIdentity refused;
  DriverStatus status;
  Diagnosis diag;
};

// `settings`: SteamVR's, if connected; tells whether it disabled the driver.
Driver Connect(vr::IVRSettings* settings) {
  Driver d;
  d.shm = OpenShm(/*driver=*/false, &d.openError, kShmName, &d.refused);
  if (d.shm) d.status = QueryStatus(d.shm);
  d.diag = Diagnose(d.shm, d.refused, d.status, settings);
  return d;
}

// Connects and requires `needed` trust; prints why not.
bool ConnectFor(Trust needed, vr::IVRSettings* settings, Driver* d) {
  *d = Connect(settings);
  if (int(d->diag.trust) >= int(needed)) return true;
  fprintf(stderr, "error: %s\n", d->diag.line.c_str());
  return false;
}

const char* TrustText(Trust t) {
  switch (t) {
    case Trust::Full: return "yes";
    case Trust::Cleanup: return "no, but resets still re-zero (restart SteamVR to finish an update)";
    default: return "no (reset sends a bare reset only)";
  }
}

void PrintR(const char* label, const double q[4], const double t[3]) {
  double y, p, r;
  ToYawPitchRoll({q[0], q[1], q[2], q[3]}, &y, &p, &r);
  printf("%s yaw %.2f pitch %.2f roll %.2f deg, t (%.3f, %.3f, %.3f) m\n", label, Deg(y), Deg(p),
         Deg(r), t[0], t[1], t[2]);
}

int Version() {
  printf("reclinectl %s\n", RECLINE_VERSION);
  return 0;
}

// This install's driver library, as SteamVR loads it.
std::string DriverLibrary() {
#ifdef _WIN32
  return ExecutableDir() + "\\driver_recline.dll";
#else
  return ExecutableDir() + "/driver_recline.so";
#endif
}

// load-test (internal): loads the driver as SteamVR would, in this process.
// Prints the system loader's error, or "ok"; status runs it as a child so a
// broken library can't take reclinectl down with it.
int LoadTest(const char* path) {
  void* lib = OpenLibrary(path);
  if (!lib) {
    printf("%s\n", LibraryError().c_str());
    return 1;
  }
  if (!LibrarySymbol(lib, "HmdDriverFactory")) {
    printf("loads, but exports no HmdDriverFactory\n");
    return 1;
  }
  printf("ok\n");
  return 0;
}

// Runs load-test in a child process; its first output line.
std::string RunLoadTest() {
#ifdef _WIN32
  const std::string exe = ExecutableDir() + "\\reclinectl.exe";
  // cmd /c drops the outer quotes, keeping the inner ones.
  const std::string cmd = "\"\"" + exe + "\" load-test \"" + DriverLibrary() + "\" 2>&1\"";
  FILE* p = _popen(cmd.c_str(), "r");
#else
  const std::string cmd = "'" + ExecutableDir() + "/reclinectl' load-test '" + DriverLibrary() + "' 2>&1";
  FILE* p = popen(cmd.c_str(), "r");
#endif
  if (!p) return "could not run the test";
  char buf[512] = {};
  std::string out = fgets(buf, sizeof buf, p) ? buf : "no output (the test crashed?)";
#ifdef _WIN32
  _pclose(p);
#else
  pclose(p);
#endif
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

void PrintRegistration() {
  const DriverRegistration reg = ReadDriverRegistration();
  if (!reg.readable) {
    printf("registered:      unknown (openvrpaths.vrpath unreadable)\n");
    return;
  }
  printf("registered:      %s\n", reg.ours ? "yes" : "NO (run the installer)");
  for (const std::string& r : reg.reclineRoots) printf("  recline root:  %s\n", r.c_str());
}

// Exit status 0 only when recline is fully usable.
int Status() {
  Version();
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Background);  // never starts SteamVR
  const bool vrOk = e == vr::VRInitError_None;
  Driver d;
  d.shm = OpenShm(/*driver=*/false, &d.openError, kShmName, &d.refused);
  HmdSnapshot h0;
  double dt = 0;
  if (d.shm) {
    ReadHmd(d.shm->hmd, &h0);
    const double t0 = MonotonicSeconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    uint64_t prevCount = h0.pose_count;
    d.status = QueryStatus(d.shm, &prevCount);
    dt = MonotonicSeconds() - t0;
  }
  d.diag = Diagnose(d.shm, d.refused, d.status, vrOk ? vr::VRSettings() : nullptr);
  if (vrOk) {
    printf("status:          %s\n", d.diag.line.c_str());
    const uint32_t pid = vr::VRApplications()->GetApplicationProcessId(kOverlayAppKey);
    if (pid) printf("overlay:         running (pid %u)\n", pid);
    else printf("overlay:         NOT running (reclinectl register, then restart SteamVR)\n");
  } else {
    printf("status:          SteamVR is not running (%s)\n", vr::VR_GetVRInitErrorAsEnglishDescription(e));
  }
  const Headset headset = vrOk ? QueryHeadset(vr::VRSystem()) : Headset{};
  if (headset.steamLink) printf("may recline:     no: %s\n", SteamLinkLine(headset).c_str());
  else printf("may recline:     %s\n", TrustText(d.diag.trust));
  PrintRegistration();
  // Should have loaded, and nothing says why not: ask the system loader.
  if (vrOk && d.diag.cause == Diagnosis::NotLoaded)
    printf("load test:       %s (%s)\n", RunLoadTest().c_str(), DriverLibrary().c_str());
  const DriverIdentity& id = d.shm ? d.status.identity : d.refused;
  if (id.layout) printf("shm:             %s layout v%u (this build: v%u)\n", kShmName, id.layout, kVersion);
  else printf("shm:             %s\n", d.openError.c_str());
  if (id.present) {
    printf("driver:          %s, pid %u\n", id.version.c_str(), id.pid);
    printf("driver file:     %s\n", id.module_path.c_str());
  }
  if (vrOk) vr::VR_Shutdown();
  if (!d.shm || !d.status.shmOk) return 1;
  const auto& st = d.status;
  const auto& s = st.state;
  printf("driver alive:    %s\n", st.driverAlive ? "yes" : "NO (pose_count not advancing)");
  printf("flags:           0x%x%s%s%s%s%s\n", s.flags,
         s.flags & FLAG_HOOK_INSTALLED ? " hook_installed" : "",
         s.flags & FLAG_TRANSFORM_ACTIVE ? " transform_active" : "",
         s.flags & FLAG_CONFLICT_DOWNSTREAM ? " conflict_downstream" : "",
         s.flags & FLAG_WRAPPED ? " wrapped" : "",
         s.flags & FLAG_HOOK_REFUSED ? " hook_refused" : "");
  printf("hmd index:       %u\n", s.hmd_index);
  printf("pose_count:      %llu (%.0f Hz)\n", (unsigned long long)st.hmd.pose_count,
         double(st.hmd.pose_count - h0.pose_count) / dt);
  if (st.hmd.time > 0) {
    printf("last HMD pose:   %.3f s ago\n", MonotonicSeconds() - st.hmd.time);
    PrintR("head (driver-world):", st.hmd.q, st.hmd.t);
  }
  PrintR("R:              ", s.R_q, s.R_t);
  printf("last cmd ack:    %u (result %u)\n", s.cmd_ack, s.cmd_result);
  return d.diag.trust == Trust::Full && !headset.steamLink ? 0 : 1;
}

int Send(ReclineShm* shm, uint32_t cmd, const Rigid* payload) {
  StateSnapshot st;
  const SendResult r = SendCommand(shm, cmd, payload, &st);
  if (r != SendResult::Ok) {
    fprintf(stderr, "error: %s\n", SendResultText(r));
    return 1;
  }
  PrintR("ok, R:", st.R_q, st.R_t);
  return 0;
}

// After a reset: re-zero the seated universe, for a trusted driver only (as in
// the overlay). `vrError`: why SteamVR is not connected, or null.
int ResetSeatedZero(const Diagnosis& diag, const vr::EVRInitError* vrError) {
  if (int(diag.trust) < int(Trust::Cleanup)) {
    printf("seated zero pose left alone: %s\n", diag.line.c_str());
    return 0;
  }
  if (vrError) {
    fprintf(stderr, "warning: transform reset, but seated zero pose not reset: VR_Init: %s\n",
            vr::VR_GetVRInitErrorAsEnglishDescription(*vrError));
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(30));  // a couple of frames
  vr::VRChaperone()->ResetZeroPose(vr::TrackingUniverseSeated);
  printf("seated zero pose reset\n");
  return 0;
}

// Reset R, then (by default) re-zero the seated universe at the current head
// pose, as the overlay does: the recenter had moved it.
int Reset(int argc, char** argv) {
  bool seatedReset = true;
  for (int i = 0; i < argc; ++i) {
    if (std::string(argv[i]) == "--no-seated-reset") {
      seatedReset = false;
    } else {
      Usage();
      return 2;
    }
  }
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Background);
  const bool vrOk = e == vr::VRInitError_None;
  // Any driver whose segment we can open gets the bare reset; the zero pose
  // only for one we trust, as in the overlay.
  Driver d = Connect(vrOk ? vr::VRSettings() : nullptr);
  int rc = 1;
  if (!d.shm) fprintf(stderr, "error: %s\n", d.diag.line.c_str());
  else rc = Send(d.shm, CMD_RESET, nullptr);
  if (rc == 0 && seatedReset) rc = ResetSeatedZero(d.diag, vrOk ? nullptr : &e);
  if (vrOk) vr::VR_Shutdown();
  return rc;
}

int Recenter(int argc, char** argv) {
  RecenterOptions o;
  bool seatedReset = true, headAxis = false;
  for (int i = 0; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--keep-position") o.keepPosition = true;
    else if (a == "--level-only") o.levelOnly = true;
    else if (a == "--no-seated-reset") seatedReset = false;
    else if (a == "--head-axis") headAxis = true;
    else if (a == "--height" && i + 1 < argc) {
      char* end;
      o.eyeHeight = strtod(argv[++i], &end);
      if (*end || !(o.eyeHeight > 0 && o.eyeHeight < 3)) {
        fprintf(stderr, "error: bad --height\n");
        return 2;
      }
    } else {
      Usage();
      return 2;
    }
  }
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Background);
  if (e != vr::VRInitError_None) {
    fprintf(stderr, "error: VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e));
    return 1;
  }
  Driver d;
  if (!ConnectFor(Trust::Full, vr::VRSettings(), &d)) {
    vr::VR_Shutdown();
    return 1;
  }
  if (const Headset h = QueryHeadset(vr::VRSystem()); h.steamLink) {
    fprintf(stderr, "error: %s\n", SteamLinkLine(h).c_str());
    vr::VR_Shutdown();
    return 1;
  }
  ReclineShm* shm = d.shm;
  if (!headAxis) {
    o.viewPitch = MeasureViewPitch(vr::VRSystem());
    printf("levelling the lens-centre view axis (%.1f deg from head axis)\n", Deg(o.viewPitch));
  }
  Rigid traw;
  std::string err;
  int rc = 0;
  if (!BuildTraw(vr::VRSystem(), o, &traw, &err)) {
    fprintf(stderr, "error: %s\n", err.c_str());
    rc = 1;
  } else {
    StateSnapshot st;
    const SendResult r = SendCommand(shm, CMD_RECENTER, &traw, &st);
    if (r != SendResult::Ok) {
      fprintf(stderr, "error: %s\n", SendResultText(r));
      rc = 1;
    } else {
      PrintR("ok, R:", st.R_q, st.R_t);
      if (seatedReset) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));  // a couple of frames
        vr::VRChaperone()->ResetZeroPose(vr::TrackingUniverseSeated);
        printf("seated zero pose reset\n");
      }
    }
  }
  vr::VR_Shutdown();
  return rc;
}

int Set(int argc, char** argv) {
  if (argc != 6) {
    Usage();
    return 2;
  }
  double v[6];
  for (int i = 0; i < 6; ++i) {
    char* end;
    v[i] = strtod(argv[i], &end);
    if (*end) {
      fprintf(stderr, "error: not a number: %s\n", argv[i]);
      return 2;
    }
  }
  Rigid R{FromYawPitchRoll(Rad(v[0]), Rad(v[1]), Rad(v[2])), {v[3], v[4], v[5]}};
  Driver d;
  if (!ConnectFor(Trust::Full, nullptr, &d)) return 1;
  return Send(d.shm, CMD_SET, &R);
}

// SteamVR's drivers and every string property of every connected device, to
// find out how a headset or controller identifies itself.
int Props() {
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Background);
  if (e != vr::VRInitError_None) {
    fprintf(stderr, "error: VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e));
    return 1;
  }
  char buf[4096];
  if (auto* dm = vr::VRDriverManager()) {
    printf("drivers:\n");
    for (uint32_t i = 0; i < dm->GetDriverCount(); ++i) {
      buf[0] = 0;
      dm->GetDriverName(i, buf, sizeof buf);
      printf("  %-28s %s\n", buf, dm->IsEnabled(i) ? "enabled" : "disabled");
    }
  }
  static const char* const kClass[] = {"invalid", "hmd", "controller", "generic tracker", "tracking reference",
                                       "display redirect"};
  auto* sys = vr::VRSystem();
  for (uint32_t d = 0; d < vr::k_unMaxTrackedDeviceCount; ++d) {
    const vr::ETrackedDeviceClass cls = sys->GetTrackedDeviceClass(d);
    if (cls == vr::TrackedDeviceClass_Invalid) continue;
    printf("device %u (%s)%s:\n", d, unsigned(cls) < 6 ? kClass[cls] : "?",
           sys->IsTrackedDeviceConnected(d) ? "" : " not connected");
    for (int id = 1; id < 10000; ++id) {
      vr::ETrackedPropertyError pe = vr::TrackedProp_Success;
      buf[0] = 0;
      sys->GetStringTrackedDeviceProperty(d, vr::ETrackedDeviceProperty(id), buf, sizeof buf, &pe);
      if (pe == vr::TrackedProp_Success && buf[0]) printf("  %4d = %s\n", id, buf);
    }
  }
  vr::VR_Shutdown();
  return 0;
}

int Poses() {
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Background);
  if (e != vr::VRInitError_None) {
    fprintf(stderr, "error: VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e));
    return 1;
  }
  const struct {
    vr::ETrackingUniverseOrigin u;
    const char* name;
  } unis[] = {{vr::TrackingUniverseStanding, "standing"},
              {vr::TrackingUniverseRawAndUncalibrated, "raw"},
              {vr::TrackingUniverseSeated, "seated"}};
  for (const auto& un : unis) {
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(un.u, 0.0f, poses, vr::k_unMaxTrackedDeviceCount);
    for (uint32_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
      const auto cls = vr::VRSystem()->GetTrackedDeviceClass(i);
      if (cls == vr::TrackedDeviceClass_Invalid || !poses[i].bPoseIsValid) continue;
      const Rigid r = FromMatrix34(poses[i].mDeviceToAbsoluteTracking);
      double y, p, ro;
      ToYawPitchRoll(r.q, &y, &p, &ro);
      printf("%-8s dev %2u class %d: yaw %7.2f pitch %7.2f roll %7.2f deg, t (%6.3f, %6.3f, %6.3f) m\n",
             un.name, i, int(cls), Deg(y), Deg(p), Deg(ro), r.t.x, r.t.y, r.t.z);
    }
  }
  // Display geometry: what the HMD pose means for the rendered views.
  for (int i = 0; i < 2; ++i) {
    const vr::EVREye eye = i ? vr::Eye_Right : vr::Eye_Left;
    const Rigid eh = FromMatrix34(vr::VRSystem()->GetEyeToHeadTransform(eye));
    double y, p, ro;
    ToYawPitchRoll(eh.q, &y, &p, &ro);
    float l, r, t, b;
    vr::VRSystem()->GetProjectionRaw(eye, &l, &r, &t, &b);
    printf("%s eye-to-head: yaw %.3f pitch %.3f roll %.3f deg, t (%.4f, %.4f, %.4f) m\n",
           e ? "right" : "left ", Deg(y), Deg(p), Deg(ro), eh.t.x, eh.t.y, eh.t.z);
    printf("%s projection tan: left %.4f right %.4f top %.4f bottom %.4f -> "
           "fov h %.1f..%.1f v %.1f..%.1f deg, vertical centre %.2f deg\n",
           e ? "right" : "left ", l, r, t, b, Deg(std::atan(l)), Deg(std::atan(r)),
           Deg(std::atan(t)), Deg(std::atan(b)), Deg(std::atan(0.5 * (t + b))));
  }
  const struct {
    vr::ETrackedDeviceProperty p;
    const char* name;
  } props[] = {{vr::Prop_LensCenterLeftU_Float, "LensCenterLeftU"},
               {vr::Prop_LensCenterLeftV_Float, "LensCenterLeftV"},
               {vr::Prop_LensCenterRightU_Float, "LensCenterRightU"},
               {vr::Prop_LensCenterRightV_Float, "LensCenterRightV"},
               {vr::Prop_ScreenshotVerticalFieldOfViewDegrees_Float, "ScreenshotVFovDeg"},
               {vr::Prop_DisplayFrequency_Float, "DisplayHz"}};
  for (const auto& pr : props) {
    vr::ETrackedPropertyError pe;
    const float v = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, pr.p, &pe);
    if (pe == vr::TrackedProp_Success) printf("%s = %.4f\n", pr.name, v);
    else printf("%s: %s\n", pr.name, vr::VRSystem()->GetPropErrorNameFromEnum(pe));
  }
  for (int i = 0; i < 2; ++i) {
    const vr::EVREye eye = i ? vr::Eye_Right : vr::Eye_Left;
    const vr::HmdMatrix44_t P = vr::VRSystem()->GetProjectionMatrix(eye, 0.1f, 100.0f);
    // Forward axis (0,0,-1) in NDC: x = -m[0][2], y = -m[1][2] (w = 1).
    printf("view pitch (lens centre vs head axis, both eyes): %.2f deg\n",
           Deg(MeasureViewPitch(vr::VRSystem())));
    printf("%s projection: m11 %.4f m12 %.4f -> forward axis at NDC y %+.4f (y up)\n",
           e ? "right" : "left ", P.m[1][1], P.m[1][2], -P.m[1][2]);
    vr::ETrackedPropertyError pe;
    const float lu = vr::VRSystem()->GetFloatTrackedDeviceProperty(
        0, e ? vr::Prop_LensCenterRightU_Float : vr::Prop_LensCenterLeftU_Float, &pe);
    const float lv = vr::VRSystem()->GetFloatTrackedDeviceProperty(
        0, e ? vr::Prop_LensCenterRightV_Float : vr::Prop_LensCenterLeftV_Float, &pe);
    for (float v : {0.0f, 0.5f, 1.0f, lv}) {
      vr::DistortionCoordinates_t dc;
      if (vr::VRSystem()->ComputeDistortion(eye, v == lv ? lu : 0.5f, v, &dc))
        printf("  distortion(u %.4f, v %.4f) -> render uv green (%.4f, %.4f)\n",
               v == lv ? lu : 0.5f, v, dc.rfGreen[0], dc.rfGreen[1]);
      else
        printf("  ComputeDistortion unavailable\n");
    }
  }
  vr::VR_Shutdown();
  return 0;
}

// Debug access to SteamVR settings (IVRSettings), for experiments.
int Setting(int argc, char** argv) {
  if (argc < 3 || (std::string(argv[0]) == "set" && argc < 4)) {
    Usage();
    return 2;
  }
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Utility);
  if (e != vr::VRInitError_None) {
    fprintf(stderr, "error: VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e));
    return 1;
  }
  auto* st = vr::VRSettings();
  const char* sec = argv[1];
  const char* key = argv[2];
  vr::EVRSettingsError se = vr::VRSettingsError_None;
  int rc = 0;
  if (std::string(argv[0]) == "get") {
    char buf[1024] = {};
    st->GetString(sec, key, buf, sizeof buf, &se);
    if (se == vr::VRSettingsError_None) printf("string: %s\n", buf);
    se = vr::VRSettingsError_None;
    const bool b = st->GetBool(sec, key, &se);
    if (se == vr::VRSettingsError_None) printf("bool:   %s\n", b ? "true" : "false");
    se = vr::VRSettingsError_None;
    const int32_t i = st->GetInt32(sec, key, &se);
    if (se == vr::VRSettingsError_None) printf("int:    %d\n", i);
    se = vr::VRSettingsError_None;
    const float f = st->GetFloat(sec, key, &se);
    if (se == vr::VRSettingsError_None) printf("float:  %g\n", f);
    else printf("(last error: %s)\n", st->GetSettingsErrorNameFromEnum(se));
  } else if (std::string(argv[0]) == "set") {
    const std::string v = argv[3];
    char* end;
    if (v == "true" || v == "false") {
      st->SetBool(sec, key, v == "true", &se);
    } else if (strtol(v.c_str(), &end, 10), *end == 0) {
      st->SetInt32(sec, key, int32_t(strtol(v.c_str(), nullptr, 10)), &se);
    } else if (strtod(v.c_str(), &end), *end == 0) {
      st->SetFloat(sec, key, float(strtod(v.c_str(), nullptr)), &se);
    } else {
      st->SetString(sec, key, v.c_str(), &se);
    }
    if (se != vr::VRSettingsError_None) {
      fprintf(stderr, "error: %s\n", st->GetSettingsErrorNameFromEnum(se));
      rc = 1;
    } else {
      printf("set %s.%s = %s\n", sec, key, v.c_str());
    }
  } else if (std::string(argv[0]) == "remove") {
    st->RemoveKeyInSection(sec, key, &se);
    if (se != vr::VRSettingsError_None) {
      fprintf(stderr, "error: %s\n", st->GetSettingsErrorNameFromEnum(se));
      rc = 1;
    } else {
      printf("removed %s.%s (back to default)\n", sec, key);
    }
  } else {
    Usage();
    rc = 2;
  }
  vr::VR_Shutdown();
  return rc;
}

int Bindings() {
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Utility);
  if (e != vr::VRInitError_None) {
    fprintf(stderr, "error: VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e));
    return 1;
  }
  const vr::EVRInputError ie = vr::VRInput()->OpenBindingUI(
      kOverlayAppKey, vr::k_ulInvalidActionSetHandle, vr::k_ulInvalidInputValueHandle,
      /*bShowOnDesktop=*/false);
  vr::VR_Shutdown();
  if (ie != vr::VRInputError_None) {
    fprintf(stderr, "error: OpenBindingUI failed (%d)\n", int(ie));
    return 1;
  }
  printf("binding editor opened in the headset\n");
  return 0;
}

// Keeps or removes the overlay's dashboard entry. Edits overlay.conf, which a
// running overlay picks up within a second; a removal waits until the
// dashboard is closed.
int Panel(int argc, char** argv) {
  const std::string a = argc == 1 ? argv[0] : "";
  if (a != "show" && a != "hide") {
    Usage();
    return 2;
  }
  OverlayOptions o;
  LoadOverlayOptions(&o);
  o.showInDashboard = a == "show";
  if (!SaveOverlayOptions(o)) {
    const std::u8string path = OverlayOptionsPath().u8string();
    fprintf(stderr, "error: could not write %s\n", reinterpret_cast<const char*>(path.c_str()));
    return 1;
  }
  printf(o.showInDashboard ? "Recline VR panel shown in the SteamVR dashboard\n"
                           : "Recline VR panel removed from the SteamVR dashboard once it is closed\n");
  return 0;
}

// --- overlay registration ------------------------------------------------------------
// A utility client needs no running SteamVR: without one it gets a short-lived
// vrserver that loads no drivers, so the installers can register with SteamVR
// stopped.

const char* AppErr(vr::EVRApplicationError e) {
  return vr::VRApplications()->GetApplicationsErrorNameFromEnum(e);
}

bool InitUtility() {
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Utility);
  if (e != vr::VRInitError_None) {
    fprintf(stderr, "error: VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e));
    return false;
  }
  return true;
}

// Adds the overlay's manifest and turns on auto-launch.
int Register() {
  const std::string manifest = InstallFile("recline_overlay.vrmanifest");
  std::error_code ec;
  if (!std::filesystem::exists(Utf8Path(manifest), ec)) {
    fprintf(stderr, "error: manifest not found: %s\n", manifest.c_str());
    return 1;
  }
  if (!InitUtility()) return 1;
  auto* apps = vr::VRApplications();
  if (apps->IsApplicationInstalled(kOverlayAppKey))
    printf("%s already registered; re-adding to pick up the current path\n", kOverlayAppKey);
  vr::EVRApplicationError e = apps->AddApplicationManifest(manifest.c_str(), false);
  if (e != vr::VRApplicationError_None) {
    fprintf(stderr, "error: AddApplicationManifest(%s): %s\n", manifest.c_str(), AppErr(e));
    vr::VR_Shutdown();
    return 1;
  }
  e = apps->SetApplicationAutoLaunch(kOverlayAppKey, true);
  if (e != vr::VRApplicationError_None) {
    fprintf(stderr, "error: SetApplicationAutoLaunch: %s\n", AppErr(e));
    vr::VR_Shutdown();
    return 1;
  }
  printf("registered %s (%s), auto-launch on\n", kOverlayAppKey, manifest.c_str());
  vr::VR_Shutdown();
  return 0;
}

// Turns off auto-launch and removes the manifest. The manifest can be
// registered while its app is not installed (SteamVR skipped it), so the path
// is removed either way.
int Unregister() {
  const std::string manifest = InstallFile("recline_overlay.vrmanifest");
  if (!InitUtility()) return 1;
  auto* apps = vr::VRApplications();
  const bool installed = apps->IsApplicationInstalled(kOverlayAppKey);
  if (installed) {
    const vr::EVRApplicationError e = apps->SetApplicationAutoLaunch(kOverlayAppKey, false);
    if (e != vr::VRApplicationError_None)
      fprintf(stderr, "warning: SetApplicationAutoLaunch(false): %s\n", AppErr(e));
  }
  const vr::EVRApplicationError e = apps->RemoveApplicationManifest(manifest.c_str());
  int rc = 0;
  if (e == vr::VRApplicationError_None) {
    printf("unregistered %s\n", kOverlayAppKey);
  } else if (!installed) {
    printf("%s is not registered\n", kOverlayAppKey);
  } else {
    fprintf(stderr, "error: RemoveApplicationManifest(%s): %s\n", manifest.c_str(), AppErr(e));
    rc = 1;
  }
  vr::VR_Shutdown();
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage();
    return 2;
  }
  const std::string c = argv[1];
  if (c == "--version") return Version();
  if (c == "status") return Status();
  if (c == "load-test" && argc == 3) return LoadTest(argv[2]);
  if (c == "recenter") return Recenter(argc - 2, argv + 2);
  if (c == "reset") return Reset(argc - 2, argv + 2);
  if (c == "set") return Set(argc - 2, argv + 2);
  if (c == "poses") return Poses();
  if (c == "props") return Props();
  if (c == "bindings") return Bindings();
  if (c == "setting") return Setting(argc - 2, argv + 2);
  if (c == "panel") return Panel(argc - 2, argv + 2);
  if (c == "register") return Register();
  if (c == "unregister") return Unregister();
  Usage();
  return 2;
}
