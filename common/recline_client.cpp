#include "recline_client.h"

#include <chrono>
#include <fstream>
#include <thread>

#include "overlay_options.h"
#include "platform.h"
#include "recline_version.h"

namespace recline {

bool GetHmdPose(vr::IVRSystem* sys, vr::ETrackingUniverseOrigin uni, Rigid* out) {
  vr::TrackedDevicePose_t poses[vr::k_unTrackedDeviceIndex_Hmd + 1];
  sys->GetDeviceToAbsoluteTrackingPose(uni, 0.0f, poses, vr::k_unTrackedDeviceIndex_Hmd + 1);
  const auto& p = poses[vr::k_unTrackedDeviceIndex_Hmd];
  if (!p.bPoseIsValid) return false;
  *out = FromMatrix34(p.mDeviceToAbsoluteTracking);
  return true;
}

double MeasureViewPitch(vr::IVRSystem* sys) {
  double sum = 0;
  int n = 0;
  for (int e = 0; e < 2; ++e) {
    const vr::EVREye eye = e ? vr::Eye_Right : vr::Eye_Left;
    const vr::HmdMatrix44_t P = sys->GetProjectionMatrix(eye, 0.1f, 100.0f);
    if (!(P.m[1][1] > 0)) continue;
    double lensV = 0.5;
    vr::ETrackedPropertyError eu, ev;
    const float lu = sys->GetFloatTrackedDeviceProperty(
        vr::k_unTrackedDeviceIndex_Hmd, e ? vr::Prop_LensCenterRightU_Float : vr::Prop_LensCenterLeftU_Float, &eu);
    const float lv = sys->GetFloatTrackedDeviceProperty(
        vr::k_unTrackedDeviceIndex_Hmd, e ? vr::Prop_LensCenterRightV_Float : vr::Prop_LensCenterLeftV_Float, &ev);
    vr::DistortionCoordinates_t dc;
    if (eu == vr::TrackedProp_Success && ev == vr::TrackedProp_Success &&
        sys->ComputeDistortion(eye, lu, lv, &dc) && dc.rfGreen[1] > 0 && dc.rfGreen[1] < 1)
      lensV = dc.rfGreen[1];
    sum += ViewPitchFromProjection(P.m[1][1], P.m[1][2], lensV);
    ++n;
  }
  return n ? sum / n : 0.0;
}

bool BuildTraw(vr::IVRSystem* sys, const RecenterOptions& o, Rigid* out, std::string* err) {
  Rigid S_cur, P_cur;
  if (!GetHmdPose(sys, vr::TrackingUniverseStanding, &S_cur) ||
      !GetHmdPose(sys, vr::TrackingUniverseRawAndUncalibrated, &P_cur)) {
    if (err) *err = "HMD pose not valid (is the headset tracking?)";
    return false;
  }
  *out = ComputeTraw(S_cur, P_cur, o);
  return true;
}

DriverStatus QueryStatus(ReclineShm* shm, uint64_t* prevPoseCount, double aliveWindowSec) {
  DriverStatus st;
  if (!shm) return st;
  st.shmOk = ReadState(shm->state, &st.state) && ReadHmd(shm->hmd, &st.hmd);
  if (!st.shmOk) return st;
  ReadIdentity(*shm, &st.identity);
  if (prevPoseCount) {
    st.driverAlive = st.hmd.pose_count != *prevPoseCount && st.hmd.pose_count != 0;
    *prevPoseCount = st.hmd.pose_count;
  } else {
    const uint64_t c0 = st.hmd.pose_count;
    std::this_thread::sleep_for(std::chrono::duration<double>(aliveWindowSec));
    ReadHmd(shm->hmd, &st.hmd);
    st.driverAlive = st.hmd.pose_count != c0;
  }
  return st;
}

namespace {

// The VERSION file of the install this client runs from; empty if unreadable.
std::string InstalledVersion() {
  std::ifstream f(Utf8Path(InstallFile("VERSION")));
  std::string v;
  std::getline(f, v);
  while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
  return v;
}

// The driver runs from the same bin/<platform>/ as this client.
bool SameInstall(const std::string& modulePath) {
  std::error_code ec;
  const bool same =
      std::filesystem::equivalent(Utf8Path(modulePath).parent_path(), Utf8Path(ExecutableDir()), ec);
  return same && !ec;
}

std::string DirOf(const std::string& path) {
  const std::u8string s = Utf8Path(path).parent_path().u8string();
  return std::string(s.begin(), s.end());
}

Diagnosis Make(Trust trust, Diagnosis::Level level, std::string line, std::string toast) {
  Diagnosis d;
  d.trust = trust;
  d.level = level;
  d.line = std::move(line);
  d.toast = std::move(toast);
  return d;
}

bool DriverSetting(vr::IVRSettings* settings, const char* key, bool* value) {
  if (!settings) return false;
  vr::EVRSettingsError e = vr::VRSettingsError_None;
  *value = settings->GetBool("driver_recline", key, &e);
  return e == vr::VRSettingsError_None;
}

// No driver to talk to; `why` says what was found instead.
Diagnosis NotRunning(vr::IVRSettings* settings, const std::string& why) {
  bool v = false;
  if (DriverSetting(settings, "blocked_by_safe_mode", &v) && v)
    return Make(Trust::None, Diagnosis::Bad,
                "Driver: blocked by SteamVR safe mode - re-enable it in Settings > Startup",
                "Recline is blocked by SteamVR safe mode");
  if (DriverSetting(settings, "enable", &v) && !v)
    return Make(Trust::None, Diagnosis::Bad,
                "Driver: turned off in SteamVR Settings > Startup > Manage add-ons",
                "Recline is turned off in SteamVR's add-ons");
  return Make(Trust::None, Diagnosis::Bad,
              "Driver: NOT LOADED (" + why + ") - restart SteamVR; if that doesn't help, see vrserver.txt",
              "Recline driver not loaded: restart SteamVR");
}

std::string DriverVersionText(const DriverIdentity& id) {
  return id.present ? id.version : std::string("older than v1.3");
}

// Newer files than the running pair are installed: the driver file was
// replaced since it was loaded, or the install's VERSION is not ours.
bool InstallChanged(const DriverIdentity& id, std::string* installed) {
  *installed = InstalledVersion();
  int64_t size = 0, mtime = 0;
  return !FileStamp(id.module_path, &size, &mtime) || size != id.module_size ||
         mtime != id.module_mtime || *installed != RECLINE_VERSION;
}

}  // namespace

Diagnosis Diagnose(const ReclineShm* shm, const std::string& openError, const DriverIdentity& refused,
                   const DriverStatus& st, vr::IVRSettings* settings) {
  const DriverIdentity& id = shm ? st.identity : refused;
  const std::string ours = RECLINE_VERSION;
  // Left in /dev/shm by an earlier SteamVR session. The pid alone could be
  // reused, so a live pose stream overrides it.
  if (id.present && !ProcessAlive(int(id.pid)) && !(shm && st.driverAlive))
    return NotRunning(settings, "shared memory is from an earlier session");
  // A driver of another build (an older one may also be left over: it can't
  // say). Checked before liveness: only a restart brings the right one.
  if (id.layout != 0 && (id.layout != kVersion || (id.present && id.version != ours)))
    return Make(Trust::None, Diagnosis::Bad,
                "Driver " + DriverVersionText(id) + " found, this is " + ours + " - restart SteamVR",
                "Recline versions differ: restart SteamVR");
  if (!shm) return NotRunning(settings, openError);
  if (!st.shmOk) return Make(Trust::None, Diagnosis::Bad, "Driver: shared memory unreadable", {});
  if (!id.present) return NotRunning(settings, "it has not started this session");
  if (!st.driverAlive) return Make(Trust::None, Diagnosis::Warn, "Driver: loaded, no headset poses yet", {});
  if (!SameInstall(id.module_path))
    return Make(Trust::None, Diagnosis::Bad,
                "Two installs: SteamVR loaded the driver from " + DirOf(id.module_path) +
                    " - uninstall one, then restart SteamVR",
                "Two Recline installs: see the Recline panel");
  // This build's driver runs, but it or this client is about to be replaced:
  // finish what is in progress, start nothing new.
  std::string installed;
  if (InstallChanged(id, &installed))
    return Make(Trust::Cleanup, Diagnosis::Bad,
                "Recline was updated" +
                    (installed.empty() || installed == ours ? std::string() : " to " + installed) +
                    " - restart SteamVR to finish",
                "Recline was updated: restart SteamVR");
  const uint32_t f = st.state.flags;
  if (f & FLAG_HOOK_REFUSED)
    return Make(Trust::Full, Diagnosis::Bad, "Driver: loaded, but the pose hook was refused (see vrserver.txt)",
                "Recline can't change poses: see vrserver.txt");
  if (!(f & FLAG_HOOK_INSTALLED))
    return Make(Trust::Full, Diagnosis::Warn, "Driver: loaded, hook not installed", {});
  return Make(Trust::Full, Diagnosis::Good, "Driver: loaded, hook installed", {});
}

const char* SendResultText(SendResult r) {
  switch (r) {
    case SendResult::Ok: return "ok";
    case SendResult::NoHmdPose: return "driver has no HMD pose yet";
    case SendResult::InvalidArgs: return "driver rejected the command (invalid arguments)";
    case SendResult::Timeout: return "driver did not acknowledge within 500 ms (driver not loaded?)";
    case SendResult::LockTimeout: return "command block is locked by another writer";
    default: return "unknown driver result";
  }
}

SendResult SendCommand(ReclineShm* shm, uint32_t cmd, const Rigid* payload,
                       StateSnapshot* stateOut, double timeoutSec) {
  double q[4] = {1, 0, 0, 0}, t[3] = {0, 0, 0};
  if (payload) {
    q[0] = payload->q.w; q[1] = payload->q.x; q[2] = payload->q.y; q[3] = payload->q.z;
    t[0] = payload->t.x; t[1] = payload->t.y; t[2] = payload->t.z;
  }
  const uint32_t id = SubmitCmd(shm->cmd, cmd, q, t);
  if (id == 0) return SendResult::LockTimeout;
  const double deadline = MonotonicSeconds() + timeoutSec;
  StateSnapshot st;
  while (MonotonicSeconds() < deadline) {
    if (ReadState(shm->state, &st) && st.cmd_ack == id) {
      if (stateOut) *stateOut = st;
      switch (st.cmd_result) {
        case RES_OK: return SendResult::Ok;
        case RES_NO_HMD_POSE: return SendResult::NoHmdPose;
        case RES_INVALID_ARGS: return SendResult::InvalidArgs;
        default: return SendResult::Other;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return SendResult::Timeout;
}

std::string InstallFile(const char* rel) {
  const std::filesystem::path root = Utf8Path(ExecutableDir()).parent_path().parent_path();
  const std::u8string s = (root / Utf8Path(rel)).make_preferred().u8string();
  return std::string(s.begin(), s.end());
}

}  // namespace recline
