// reclinectl: command-line client for driver_recline (SPEC §5.4).
//
//   reclinectl status
//   reclinectl recenter [--keep-position] [--level-only] [--height M] [--no-seated-reset]
//   reclinectl reset
//   reclinectl set <yaw> <pitch> <roll> <x> <y> <z>     (degrees, meters)
//   reclinectl poses      (prints device poses per tracking universe, for testing)

#include <openvr.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "recline_client.h"

using namespace recline;

namespace {

void Usage() {
  fprintf(stderr,
          "usage: reclinectl status\n"
          "       reclinectl recenter [--keep-position] [--level-only] [--height M]\n"
          "                           [--no-seated-reset]\n"
          "       reclinectl reset\n"
          "       reclinectl poses        (HMD/controller poses as apps see them)\n"
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

int Recenter(int argc, char** argv) {
  RecenterOptions o;
  bool seatedReset = true;
  for (int i = 0; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--keep-position") o.keepPosition = true;
    else if (a == "--level-only") o.levelOnly = true;
    else if (a == "--no-seated-reset") seatedReset = false;
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
  vr::VR_Shutdown();
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
  if (c == "reset") return Send(CMD_RESET, nullptr);
  if (c == "set") return Set(argc - 2, argv + 2);
  if (c == "poses") return Poses();
  Usage();
  return 2;
}
