#include "recline_client.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <vector>
#include <thread>

#include "overlay_options.h"
#include "platform.h"
#include "recline_version.h"
#include "vrcore/vrpathregistry_public.h"

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

namespace fs = std::filesystem;

// The VERSION file of the install this client runs from; empty if unreadable.
std::string InstalledVersion() {
  std::ifstream f(Utf8Path(InstallFile("VERSION")));
  std::string v;
  std::getline(f, v);
  while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
  return v;
}

// This client's install root (bin/<platform>/ is two levels below it).
fs::path OwnRoot() { return Utf8Path(ExecutableDir()).parent_path().parent_path(); }

bool SameDir(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  const bool same = fs::equivalent(a, b, ec);
  return same && !ec;
}

std::string Text(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

// The driver runs from the same bin/<platform>/ as this client.
bool SameInstall(const std::string& modulePath) {
  return SameDir(Utf8Path(modulePath).parent_path(), Utf8Path(ExecutableDir()));
}

// A driver root whose manifest names the driver "recline".
bool IsReclineRoot(const fs::path& root) {
  std::ifstream f(root / "driver.vrdrivermanifest");
  const std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const size_t key = s.find("\"name\"");
  return key != std::string::npos && s.find("\"recline\"", key) != std::string::npos;
}

Diagnosis Make(Diagnosis::Cause cause, Trust trust, Diagnosis::Level level, std::string line,
               std::string toast) {
  Diagnosis d;
  d.cause = cause;
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

// Another registered Recline root than this client's, or empty.
std::string OtherRegisteredRoot(const DriverRegistration& reg) {
  for (const std::string& r : reg.reclineRoots)
    if (!SameDir(Utf8Path(r), OwnRoot())) return r;
  return {};
}

Diagnosis TwoInstalls(const std::string& otherDir) {
  return Make(Diagnosis::OtherInstall, Trust::None, Diagnosis::Bad,
              "Two installs: SteamVR uses Recline VR from " + otherDir + " - uninstall one, then restart SteamVR",
              "Two Recline VR installs: see the Recline VR panel");
}

// No driver runs this session.
Diagnosis NotRunning(vr::IVRSettings* settings) {
  bool v = false;
  if (DriverSetting(settings, "blocked_by_safe_mode", &v) && v)
    return Make(Diagnosis::SafeMode, Trust::None, Diagnosis::Bad,
                "Driver: blocked by SteamVR safe mode - re-enable it in Settings > Startup",
                "Recline VR is blocked by SteamVR safe mode");
  if (DriverSetting(settings, "enable", &v) && !v)
    return Make(Diagnosis::Disabled, Trust::None, Diagnosis::Bad,
                "Driver: turned off in SteamVR Settings > Startup > Manage add-ons",
                "Recline VR is turned off in SteamVR's add-ons");
  const DriverRegistration reg = ReadDriverRegistration();
  if (reg.readable && !reg.ours) {
    const std::string other = OtherRegisteredRoot(reg);
    if (!other.empty())
      return Make(Diagnosis::NotRegistered, Trust::None, Diagnosis::Bad,
                  "Driver: SteamVR has Recline VR registered from " + other + ", not from here - run this install's installer",
                  "Recline VR is registered from another folder");
    return Make(Diagnosis::NotRegistered, Trust::None, Diagnosis::Bad,
                "Driver: not registered with SteamVR - run the installer", "Recline VR is not installed in SteamVR");
  }
  return Make(Diagnosis::NotLoaded, Trust::None, Diagnosis::Bad,
              "Driver: not loaded - restart SteamVR (if it persists: reclinectl status)",
              "Recline VR driver not loaded: restart SteamVR");
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

DriverRegistration ReadDriverRegistration() {
  static DriverRegistration cached;
  static double readAt = -1e9;
  const double now = MonotonicSeconds();
  if (now - readAt < 2.0) return cached;
  readAt = now;
  DriverRegistration& reg = cached;
  reg = DriverRegistration{};
  std::string runtime, config, log;
  std::vector<std::string> drivers;
  reg.readable = CVRPathRegistry_Public::GetPaths(&runtime, &config, &log, nullptr, nullptr, &drivers);
  if (!reg.readable) return reg;
  for (const std::string& d : drivers) {
    if (!IsReclineRoot(Utf8Path(d))) continue;
    reg.reclineRoots.push_back(d);
    if (SameDir(Utf8Path(d), OwnRoot())) reg.ours = true;
  }
  return reg;
}

Diagnosis Diagnose(const ReclineShm* shm, const DriverIdentity& refused, const DriverStatus& st,
                   vr::IVRSettings* settings) {
  const DriverIdentity& id = shm ? st.identity : refused;
  const std::string ours = RECLINE_VERSION;
  // Left in /dev/shm by an earlier SteamVR session. The pid alone could be
  // reused, so a live pose stream overrides it.
  if (id.present && !ProcessAlive(int(id.pid)) && !(shm && st.driverAlive)) return NotRunning(settings);
  // The driver of another install, whatever its version: a restart would
  // bring the same one back.
  if (id.present && !SameInstall(id.module_path))
    return TwoInstalls(Text(Utf8Path(id.module_path).parent_path().parent_path().parent_path()));
  // A driver of another build. An older one can't say where it is from (or
  // whether it still runs); if another install is registered, it is from there.
  if (id.layout != 0 && (id.layout != kVersion || (id.present && id.version != ours))) {
    if (!id.present) {
      const std::string other = OtherRegisteredRoot(ReadDriverRegistration());
      if (!other.empty()) return TwoInstalls(other);
    }
    return Make(Diagnosis::OtherVersion, Trust::None, Diagnosis::Bad,
                "Driver " + (id.present ? id.version : std::string("older than v1.3")) + " found, this is " +
                    ours + " - restart SteamVR",
                "Recline VR versions differ: restart SteamVR");
  }
  if (!shm || !id.present) return NotRunning(settings);
  if (!st.shmOk) return Make(Diagnosis::Unreadable, Trust::None, Diagnosis::Bad, "Driver: shared memory unreadable", {});
  if (!st.driverAlive)
    return Make(Diagnosis::NoPoses, Trust::None, Diagnosis::Warn, "Driver: loaded, no headset poses yet", {});
  // This build's driver runs, but it or this client is about to be replaced:
  // finish what is in progress, start nothing new.
  std::string installed;
  if (InstallChanged(id, &installed))
    return Make(Diagnosis::Updated, Trust::Cleanup, Diagnosis::Bad,
                "Recline VR was updated" +
                    (installed.empty() || installed == ours ? std::string() : " to " + installed) +
                    " - restart SteamVR to finish",
                "Recline VR was updated: restart SteamVR");
  const uint32_t f = st.state.flags;
  // The driver sets HOOK_INSTALLED or HOOK_REFUSED before its first state
  // write, and its poses (liveness) only flow after that: a live driver
  // without HOOK_INSTALLED has refused.
  if (!(f & FLAG_HOOK_INSTALLED))
    return Make(Diagnosis::HookRefused, Trust::Full, Diagnosis::Bad,
                "Driver: loaded, but the pose hook was refused (see vrserver.txt)",
                "Recline VR isn't working: see the Recline VR panel");
  // SteamVR loads the first of two registered installs and skips the other;
  // removing or re-registering one would bring the other back unannounced.
  const std::string other = OtherRegisteredRoot(ReadDriverRegistration());
  if (!other.empty())
    return Make(Diagnosis::AlsoRegistered, Trust::Full, Diagnosis::Warn,
                "Driver: loaded; Recline VR is also registered from " + other + " (SteamVR skips it) - uninstall it",
                {});
  return Make(Diagnosis::Ok, Trust::Full, Diagnosis::Good, "Driver: loaded, hook installed", {});
}

namespace {

std::string HmdString(vr::IVRSystem* sys, vr::ETrackedDeviceProperty prop) {
  char buf[256] = {};
  vr::ETrackedPropertyError e = vr::TrackedProp_Success;
  sys->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, prop, buf, sizeof buf, &e);
  return e == vr::TrackedProp_Success ? buf : "";
}

}  // namespace

Headset QueryHeadset(vr::IVRSystem* sys) {
  Headset h;
  if (!sys) return h;
  // The literal local driver: Prop_TrackingSystemName_String is what the
  // headset presents itself as ("cv" for a streamed Frame).
  h.steamLink = HmdString(sys, vr::Prop_ActualTrackingSystemName_String) == "vrlink";
  h.frame = HmdString(sys, vr::Prop_ResourceRoot_String) == "frame_hmd" ||
            HmdString(sys, vr::Prop_ControllerType_String) == "frame_hmd";
  return h;
}

std::string SteamLinkLine(const Headset& h) {
  return h.frame ? "Recline VR doesn't work over Steam Link: install it on the Frame instead "
                   "(it covers streamed games too)"
                 : "Recline VR doesn't work over Steam Link streaming";
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
