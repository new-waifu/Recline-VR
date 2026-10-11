#include "recline_client.h"

#include <chrono>
#include <thread>

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

}  // namespace recline
