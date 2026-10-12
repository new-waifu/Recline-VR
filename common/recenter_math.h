// Recenter target math (see docs/DESIGN.md, "Spaces and recenter math"),
// independent of the OpenVR client API so
// it can be unit-tested together with the driver-side pose math.
#pragma once

#include "recline_math.h"

namespace recline {

struct RecenterOptions {
  bool levelOnly = false;     // rotation: Full (false) | Level only (true)
  bool keepPosition = false;  // position: Target height (false) | Keep position (true)
  double eyeHeight = 1.20;    // m, for Target height
  // Pitch of the reference view axis relative to the head pose's forward axis
  // (radians, negative = below). 0 levels the head axis itself. On the Steam
  // Frame the lens optical axis is ~14° below the head axis (asymmetric
  // frustum), so levelling the head axis leaves a straight-ahead gaze looking
  // down; see ViewPitchFromProjection.
  double viewPitch = 0.0;
};

// Pure math of building the target, separated for unit tests.
//   S_cur: HMD in TrackingUniverseStanding
//   P_cur: HMD in TrackingUniverseRawAndUncalibrated
// Returns Traw = Z ∘ S_tgt with Z = P_cur ∘ S_cur⁻¹ (raw-from-standing).
inline Rigid ComputeTraw(const Rigid& S_cur, const Rigid& P_cur, const RecenterOptions& o) {
  const Rigid Z = Compose(P_cur, Inverse(S_cur));
  // Work with the view axis V = head ∘ pitch(viewPitch), level that, and
  // convert back to a head orientation.
  const Quat toView = AxisAngle({1, 0, 0}, o.viewPitch);
  Rigid S_tgt;
  S_tgt.q = Normalize((o.levelOnly ? LevelOnly(S_cur.q * toView) : Quat{}) * Conj(toView));
  S_tgt.t = o.keepPosition ? S_cur.t : Vec3{0, o.eyeHeight, 0};
  return Compose(Z, S_tgt);
}

// Pitch (radians) of the view direction seen through the lens centre,
// relative to the head's forward axis, from one eye's projection.
//   m11, m12: GetProjectionMatrix(eye).m[1][1] / m[1][2] (y-up NDC)
//   lensV:    render-texture v (0 = top) seen at the lens centre, i.e.
//             ComputeDistortion(eye, LensCenterU, LensCenterV).rfGreen[1];
//             0.5 when unknown (the render target centre).
inline double ViewPitchFromProjection(double m11, double m12, double lensV) {
  const double yNdc = 1.0 - 2.0 * lensV;
  // y_ndc = m11 * tan - m12 for a direction (0, tan, -1).
  return std::atan((yNdc + m12) / m11);
}

// Tilt (radians) of the lens-centre view frame from upright: the angle between
// its up axis and world up. Pitch and roll both count, so lying on the back,
// side or front all read about 90°. `head` must be in a gravity-aligned space
// (+Y up), e.g. the driver-world head pose H.
inline double ViewTilt(Quat head, double viewPitch) {
  const Vec3 up = Rotate(head * AxisAngle({1, 0, 0}, viewPitch), {0, 1, 0});
  return std::acos(std::fmax(-1.0, std::fmin(1.0, up.y)));
}

}  // namespace recline
