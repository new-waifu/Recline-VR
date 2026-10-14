// DriverPose_t helpers shared by the driver and the unit tests.
#pragma once

#include <openvr_driver.h>

#include "recline_math.h"

namespace recline {

// A driver quaternion. Some drivers leave a rotation all zero (SteamVR's null
// driver: its device rotation, qRotation), which SteamVR takes as no rotation;
// so does this.
inline Quat DriverQuat(const vr::HmdQuaternion_t& q) {
  const Quat r{q.w, q.x, q.y, q.z};
  return Norm(r) > 1e-12 ? r : Quat{};
}

// Head pose in driver-world: world-from-driver ∘ driver-from-tracker ∘ head.
inline Rigid HeadFromDriverPose(const vr::DriverPose_t& d) {
  const Quat qw = DriverQuat(d.qWorldFromDriverRotation);
  const Quat qr = DriverQuat(d.qRotation);
  const Quat qh = DriverQuat(d.qDriverFromHeadRotation);
  const Vec3 tw{d.vecWorldFromDriverTranslation[0], d.vecWorldFromDriverTranslation[1],
                d.vecWorldFromDriverTranslation[2]};
  const Vec3 th{d.vecDriverFromHeadTranslation[0], d.vecDriverFromHeadTranslation[1],
                d.vecDriverFromHeadTranslation[2]};
  const Vec3 p{d.vecPosition[0], d.vecPosition[1], d.vecPosition[2]};
  Rigid h;
  h.q = Normalize(qw * qr * qh);
  h.t = Rotate(qw, Rotate(qr, th) + p) + tw;
  return h;
}

// Same, without world-from-driver (used to tell whether GetRawTrackedDevicePoses
// returns poses before or after world-from-driver).
inline Rigid HeadInDriverSpace(const vr::DriverPose_t& d) {
  vr::DriverPose_t c = d;
  c.qWorldFromDriverRotation = {1, 0, 0, 0};
  c.vecWorldFromDriverTranslation[0] = c.vecWorldFromDriverTranslation[1] =
      c.vecWorldFromDriverTranslation[2] = 0;
  return HeadFromDriverPose(c);
}

// pose' = T ∘ pose. Touches only the world-from-driver fields: velocities,
// accelerations and driver-from-head live in driver space and follow along.
inline void ApplyWorldTransform(const Rigid& T, vr::DriverPose_t* p) {
  // SteamVR multiplies the world-from-driver, device and driver-from-head
  // rotations itself, and only then reads an all-zero product as no rotation:
  // a zero device or driver-from-head rotation would drop T's rotation. Write
  // the identity they stand for.
  for (vr::HmdQuaternion_t* q : {&p->qRotation, &p->qDriverFromHeadRotation}) {
    const Quat d = DriverQuat(*q);
    *q = {d.w, d.x, d.y, d.z};
  }
  const Quat qw = DriverQuat(p->qWorldFromDriverRotation);
  const Vec3 tw{p->vecWorldFromDriverTranslation[0], p->vecWorldFromDriverTranslation[1],
                p->vecWorldFromDriverTranslation[2]};
  const Quat q = Normalize(T.q * qw);
  const Vec3 t = Rotate(T.q, tw) + T.t;
  p->qWorldFromDriverRotation = {q.w, q.x, q.y, q.z};
  p->vecWorldFromDriverTranslation[0] = t.x;
  p->vecWorldFromDriverTranslation[1] = t.y;
  p->vecWorldFromDriverTranslation[2] = t.z;
}

}  // namespace recline
