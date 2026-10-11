// DriverPose_t helpers shared by the drivers and the unit tests (SPEC §4.3/§4.4).
#pragma once

#include <openvr_driver.h>

#include "recline_math.h"

namespace recline {

// Head pose in driver-world: world-from-driver ∘ driver-from-tracker ∘ head.
inline Rigid HeadFromDriverPose(const vr::DriverPose_t& d) {
  const Quat qw{d.qWorldFromDriverRotation.w, d.qWorldFromDriverRotation.x,
                d.qWorldFromDriverRotation.y, d.qWorldFromDriverRotation.z};
  const Quat qr{d.qRotation.w, d.qRotation.x, d.qRotation.y, d.qRotation.z};
  const Quat qh{d.qDriverFromHeadRotation.w, d.qDriverFromHeadRotation.x,
                d.qDriverFromHeadRotation.y, d.qDriverFromHeadRotation.z};
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

// Same, without world-from-driver (used to classify GetRawTrackedDevicePoses
// semantics, SPEC §10.5).
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
  const Quat qw{p->qWorldFromDriverRotation.w, p->qWorldFromDriverRotation.x,
                p->qWorldFromDriverRotation.y, p->qWorldFromDriverRotation.z};
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
