// Quaternion / rigid-transform math shared by driver, overlay and CLI.
// Conventions follow OpenVR: right-handed, +X right, +Y up, -Z forward.
// Quaternions are (w, x, y, z), double precision.
#pragma once

#include <cmath>

namespace recline {

struct Vec3 {
  double x = 0, y = 0, z = 0;
};
struct Quat {
  double w = 1, x = 0, y = 0, z = 0;
};
// Rigid transform: p' = q ⊗ p + t
struct Rigid {
  Quat q;
  Vec3 t;
};

constexpr double kPi = 3.14159265358979323846;
inline double Deg(double rad) { return rad * 180.0 / kPi; }
inline double Rad(double deg) { return deg * kPi / 180.0; }

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double Length(Vec3 a) { return std::sqrt(Dot(a, a)); }

inline Quat operator*(Quat a, Quat b) {
  return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
          a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
inline Quat Conj(Quat q) { return {q.w, -q.x, -q.y, -q.z}; }
inline double Norm(Quat q) { return std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z); }
inline Quat Normalize(Quat q) {
  const double n = Norm(q);
  if (!(n > 1e-12)) return Quat{};
  return {q.w / n, q.x / n, q.y / n, q.z / n};
}
inline Vec3 Rotate(Quat q, Vec3 v) {
  // v' = v + 2w(u×v) + 2u×(u×v), u = (x,y,z)
  const Vec3 u{q.x, q.y, q.z};
  const Vec3 c = Cross(u, v) * 2.0;
  return v + c * q.w + Cross(u, c);
}
inline Quat AxisAngle(Vec3 axis, double rad) {
  const double l = Length(axis);
  if (!(l > 1e-12)) return Quat{};
  const double s = std::sin(rad * 0.5) / l;
  return {std::cos(rad * 0.5), axis.x * s, axis.y * s, axis.z * s};
}
// Angle of the rotation between a and b, radians in [0, pi].
inline double AngleBetween(Quat a, Quat b) {
  const Quat d = Conj(Normalize(a)) * Normalize(b);
  // atan2 form: accurate for tiny angles, unlike acos(|w|).
  return 2.0 * std::atan2(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), std::fabs(d.w));
}

inline Rigid Compose(const Rigid& a, const Rigid& b) {  // a ∘ b
  return {Normalize(a.q * b.q), Rotate(a.q, b.t) + a.t};
}
inline Rigid Inverse(const Rigid& a) {
  const Quat qi = Conj(a.q);
  return {qi, Rotate(qi, a.t) * -1.0};
}
inline bool IsIdentity(const Rigid& r, double eps = 1e-12) {
  return std::fabs(r.q.x) < eps && std::fabs(r.q.y) < eps && std::fabs(r.q.z) < eps &&
         std::fabs(r.t.x) < eps && std::fabs(r.t.y) < eps && std::fabs(r.t.z) < eps;
}
inline bool IsFinite(const Rigid& r) {
  return std::isfinite(r.q.w) && std::isfinite(r.q.x) && std::isfinite(r.q.y) &&
         std::isfinite(r.q.z) && std::isfinite(r.t.x) && std::isfinite(r.t.y) &&
         std::isfinite(r.t.z);
}
// Sanity check for transforms crossing a process boundary.
inline bool IsSaneRigid(const Rigid& r, double maxTranslation = 1000.0) {
  if (!IsFinite(r)) return false;
  if (std::fabs(Norm(r.q) - 1.0) > 1e-3) return false;
  return Length(r.t) <= maxTranslation;
}

// Minimal rotation taking unit vector a onto unit vector b. For a ≈ -b, the
// rotation is 180° about `fallbackAxis` (must be ⟂ a).
inline Quat RotationBetween(Vec3 a, Vec3 b, Vec3 fallbackAxis) {
  const double d = Dot(a, b);
  if (d < -1.0 + 1e-9) return AxisAngle(fallbackAxis, kPi);
  const Vec3 c = Cross(a, b);
  return Normalize(Quat{1.0 + d, c.x, c.y, c.z});
}

// "Level only": remove pitch and roll by applying the minimal world rotation
// that maps the orientation's up vector to world +Y. The resulting heading is
// whatever the forward vector ends up as.
inline Quat LevelOnly(Quat q) {
  q = Normalize(q);
  const Vec3 up = Rotate(q, Vec3{0, 1, 0});
  const Vec3 right = Rotate(q, Vec3{1, 0, 0});  // ⟂ up: pitch over when upside down
  return Normalize(RotationBetween(up, Vec3{0, 1, 0}, right) * q);
}

// Yaw (about +Y), pitch (about +X), roll (about +Z), applied as
// q = yaw * pitch * roll. Radians.
inline Quat FromYawPitchRoll(double yaw, double pitch, double roll) {
  return Normalize(AxisAngle({0, 1, 0}, yaw) * AxisAngle({1, 0, 0}, pitch) *
                   AxisAngle({0, 0, 1}, roll));
}
inline void ToYawPitchRoll(Quat q, double* yaw, double* pitch, double* roll) {
  q = Normalize(q);
  // Inverse of R = Ry(yaw) Rx(pitch) Rz(roll).
  const double sp = 2.0 * (q.w * q.x - q.y * q.z);
  *pitch = std::asin(sp > 1 ? 1 : (sp < -1 ? -1 : sp));
  *yaw = std::atan2(2.0 * (q.w * q.y + q.x * q.z), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
  *roll = std::atan2(2.0 * (q.w * q.z + q.x * q.y), 1.0 - 2.0 * (q.x * q.x + q.z * q.z));
}

// 3x4 row-major [R | t] (as in HmdMatrix34_t) <-> Rigid.
template <class M34>
inline Rigid FromMatrix34(const M34& mat) {
  const auto& m = mat.m;
  const double m00 = m[0][0], m01 = m[0][1], m02 = m[0][2];
  const double m10 = m[1][0], m11 = m[1][1], m12 = m[1][2];
  const double m20 = m[2][0], m21 = m[2][1], m22 = m[2][2];
  Quat q;
  const double tr = m00 + m11 + m22;
  if (tr > 0) {
    const double s = std::sqrt(tr + 1.0) * 2;
    q = {0.25 * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s};
  } else if (m00 > m11 && m00 > m22) {
    const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
    q = {(m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s};
  } else if (m11 > m22) {
    const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
    q = {(m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s};
  } else {
    const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
    q = {(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s};
  }
  if (q.w < 0) q = {-q.w, -q.x, -q.y, -q.z};
  return {Normalize(q), {m[0][3], m[1][3], m[2][3]}};
}

template <class M34>
inline void ToMatrix34(const Rigid& r, M34* out) {
  const Quat q = Normalize(r.q);
  const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  auto& m = out->m;
  m[0][0] = 1 - 2 * (yy + zz); m[0][1] = 2 * (xy - wz);     m[0][2] = 2 * (xz + wy);     m[0][3] = r.t.x;
  m[1][0] = 2 * (xy + wz);     m[1][1] = 1 - 2 * (xx + zz); m[1][2] = 2 * (yz - wx);     m[1][3] = r.t.y;
  m[2][0] = 2 * (xz - wy);     m[2][1] = 2 * (yz + wx);     m[2][2] = 1 - 2 * (xx + yy); m[2][3] = r.t.z;
}

}  // namespace recline
