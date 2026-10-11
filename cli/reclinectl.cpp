// reclinectl: command-line client for driver_recline.
//
//   reclinectl status
//   reclinectl recenter [--keep-position] [--level-only] [--height M] [--no-seated-reset]
//     (undocumented: --head-axis levels the head pose's forward axis instead of the
//      lens-centre view axis; diagnostic only)
//   reclinectl reset [--no-seated-reset]
//   reclinectl set <yaw> <pitch> <roll> <x> <y> <z>     (degrees, meters)
//   reclinectl poses      (prints device poses per tracking universe, for testing)

#include <openvr.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

#include "recline_client.h"

using namespace recline;

namespace {

void Usage() {
  fprintf(stderr,
          "usage: reclinectl status\n"
          "       reclinectl recenter [--keep-position] [--level-only] [--height M]\n"
          "                           [--no-seated-reset]\n"
          "       reclinectl reset [--no-seated-reset]\n"
          "       reclinectl poses        (HMD/controller poses as apps see them)\n"
          "       reclinectl bindings     (open the controller binding editor in the headset)\n"
          "       reclinectl setting get <section> <key>\n"
          "       reclinectl setting set <section> <key> <bool|int|float|string>\n"
          "       reclinectl set <yaw> <pitch> <roll> <x> <y> <z>   (degrees, meters)\n");
}

ReclineShm* Open() {
  std::string err;
  ReclineShm* shm = OpenShm(/*takeOverUninitialized=*/false, &err);
  if (!shm) fprintf(stderr, "error: %s\n", err.c_str());
  return shm;
}

void PrintR(const char* label, const double q[4], const double t[3]) {
  double y, p, r;
  ToYawPitchRoll({q[0], q[1], q[2], q[3]}, &y, &p, &r);
  printf("%s yaw %.2f pitch %.2f roll %.2f deg, t (%.3f, %.3f, %.3f) m\n", label, Deg(y), Deg(p),
         Deg(r), t[0], t[1], t[2]);
}

int Status() {
  ReclineShm* shm = Open();
  if (!shm) return 1;
  HmdSnapshot h0;
  ReadHmd(shm->hmd, &h0);
  const double t0 = MonotonicSeconds();
  usleep(500000);
  uint64_t prevCount = h0.pose_count;
  DriverStatus st = QueryStatus(shm, &prevCount);
  const double dt = MonotonicSeconds() - t0;
  if (!st.shmOk) {
    fprintf(stderr, "error: could not read a consistent snapshot\n");
    return 1;
  }
  const auto& s = st.state;
  printf("shm:             %s v%u\n", kShmName, __atomic_load_n(&shm->version, __ATOMIC_RELAXED));
  printf("driver alive:    %s\n", st.driverAlive ? "yes" : "NO (pose_count not advancing)");
  printf("flags:           0x%x%s%s%s%s%s\n", s.flags,
         s.flags & FLAG_HOOK_INSTALLED ? " hook_installed" : "",
         s.flags & FLAG_TRANSFORM_ACTIVE ? " transform_active" : "",
         s.flags & FLAG_CONFLICT_DOWNSTREAM ? " conflict_downstream" : "",
         s.flags & FLAG_CONFLICT_WRAPPED ? " conflict_wrapped" : "",
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
  return st.driverAlive ? 0 : 1;
}

int Send(uint32_t cmd, const Rigid* payload) {
  ReclineShm* shm = Open();
  if (!shm) return 1;
  StateSnapshot st;
  const SendResult r = SendCommand(shm, cmd, payload, &st);
  if (r != SendResult::Ok) {
    fprintf(stderr, "error: %s\n", SendResultText(r));
    return 1;
  }
  PrintR("ok, R:", st.R_q, st.R_t);
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
  const int rc = Send(CMD_RESET, nullptr);
  if (rc != 0 || !seatedReset) return rc;
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Background);
  if (e != vr::VRInitError_None) {
    fprintf(stderr, "warning: transform reset, but seated zero pose not reset: VR_Init: %s\n",
            vr::VR_GetVRInitErrorAsEnglishDescription(e));
    return 1;
  }
  usleep(30000);  // a couple of frames
  vr::VRChaperone()->ResetZeroPose(vr::TrackingUniverseSeated);
  printf("seated zero pose reset\n");
  vr::VR_Shutdown();
  return 0;
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
  ReclineShm* shm = Open();
  if (!shm) return 1;
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, vr::VRApplication_Background);
  if (e != vr::VRInitError_None) {
    fprintf(stderr, "error: VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e));
    return 1;
  }
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
        usleep(30000);  // a couple of frames
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
  return Send(CMD_SET, &R);
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
  for (int e = 0; e < 2; ++e) {
    const vr::EVREye eye = e ? vr::Eye_Right : vr::Eye_Left;
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
  for (int e = 0; e < 2; ++e) {
    const vr::EVREye eye = e ? vr::Eye_Right : vr::Eye_Left;
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
      "user.recline.overlay", vr::k_ulInvalidActionSetHandle, vr::k_ulInvalidInputValueHandle,
      /*bShowOnDesktop=*/false);
  vr::VR_Shutdown();
  if (ie != vr::VRInputError_None) {
    fprintf(stderr, "error: OpenBindingUI failed (%d)\n", int(ie));
    return 1;
  }
  printf("binding editor opened in the headset\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage();
    return 2;
  }
  const std::string c = argv[1];
  if (c == "status") return Status();
  if (c == "recenter") return Recenter(argc - 2, argv + 2);
  if (c == "reset") return Reset(argc - 2, argv + 2);
  if (c == "set") return Set(argc - 2, argv + 2);
  if (c == "poses") return Poses();
  if (c == "bindings") return Bindings();
  if (c == "setting") return Setting(argc - 2, argv + 2);
  Usage();
  return 2;
}
