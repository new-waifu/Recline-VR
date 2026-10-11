// Host unit tests for the transform math and the §5.3 recenter pipeline,
// including the driver-side half (R = Traw ∘ H⁻¹ applied to world-from-driver).
#include <cstdio>
#include <cstdlib>
#include <random>

#include "../driver/pose_math.h"
#include "recenter_math.h"

using namespace recline;

static int g_fail = 0;
#define CHECK(cond, ...)                                          \
  do {                                                            \
    if (!(cond)) {                                                \
      ++g_fail;                                                   \
      fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
      fprintf(stderr, __VA_ARGS__);                               \
      fprintf(stderr, "\n");                                      \
    }                                                             \
  } while (0)

static std::mt19937_64 rng(12345);
static double U(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }
static Quat RandQ() { return Normalize({U(-1, 1), U(-1, 1), U(-1, 1), U(-1, 1)}); }
static Vec3 RandV(double s = 2) { return {U(-s, s), U(-s, s), U(-s, s)}; }
static Rigid RandR() { return {RandQ(), RandV()}; }

static bool Near(const Rigid& a, const Rigid& b, double pos = 1e-9, double rot = 1e-9) {
  return Length(a.t - b.t) < pos && AngleBetween(a.q, b.q) < rot;
}

struct M34 {
  float m[3][4];
};
struct M34d {
  double m[3][4];
};

static void TestBasics() {
  for (int i = 0; i < 1000; ++i) {
    const Rigid a = RandR(), b = RandR(), c = RandR();
    CHECK(Near(Compose(a, Inverse(a)), Rigid{}), "a∘a⁻¹");
    CHECK(Near(Compose(Compose(a, b), c), Compose(a, Compose(b, c))), "associativity");
    const Vec3 v = RandV();
    const Vec3 r1 = Rotate(a.q, v);
    M34d m;
    ToMatrix34(Rigid{a.q, {}}, &m);
    const Vec3 r2{m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
                  m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
                  m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z};
    CHECK(Length(r1 - r2) < 1e-12, "matrix vs quaternion rotate");
    CHECK(Near(FromMatrix34(m), Rigid{a.q, {}}, 1e-9, 1e-7), "matrix roundtrip");
    M34 mf;
    ToMatrix34(a, &mf);
    CHECK(Near(FromMatrix34(mf), a, 1e-6, 1e-5), "float matrix roundtrip");

    const double y = U(-3.1, 3.1), p = U(-1.5, 1.5), r = U(-3.1, 3.1);
    double y2, p2, r2b;
    ToYawPitchRoll(FromYawPitchRoll(y, p, r), &y2, &p2, &r2b);
    CHECK(std::fabs(y - y2) < 1e-9 && std::fabs(p - p2) < 1e-9 && std::fabs(r - r2b) < 1e-9,
          "ypr roundtrip %f %f %f -> %f %f %f", y, p, r, y2, p2, r2b);
  }
  // Pitch +90 about X maps forward (-Z) to up (+Y).
  const Vec3 f = Rotate(AxisAngle({1, 0, 0}, kPi / 2), {0, 0, -1});
  CHECK(Length(f - Vec3{0, 1, 0}) < 1e-12, "pitch convention");
}

static void TestLevelOnly() {
  for (int i = 0; i < 1000; ++i) {
    const Quat q = RandQ();
    const Quat l = LevelOnly(q);
    const Vec3 up = Rotate(l, {0, 1, 0});
    CHECK(Length(up - Vec3{0, 1, 0}) < 1e-9, "level-only up");
    // Minimal: the applied rotation (l q⁻¹) has an axis in the horizontal plane.
    const Quat d = l * Conj(q);
    CHECK(std::fabs(d.y) < 1e-9, "level-only correction is not minimal (has yaw), d.y=%g", d.y);
  }
  // Already level: unchanged (heading kept).
  const Quat yawOnly = AxisAngle({0, 1, 0}, 0.7);
  CHECK(AngleBetween(LevelOnly(yawOnly), yawOnly) < 1e-9, "level q unchanged");
  // Upside down: still produces a valid level pose.
  const Quat flipped = AxisAngle({0, 0, 1}, kPi);
  CHECK(Length(Rotate(LevelOnly(flipped), {0, 1, 0}) - Vec3{0, 1, 0}) < 1e-9, "upside down");
}

static vr::DriverPose_t RandDriverPose() {
  vr::DriverPose_t d{};
  auto setQ = [](vr::HmdQuaternion_t& o, Quat q) { o = {q.w, q.x, q.y, q.z}; };
  auto setV = [](double* o, Vec3 v) { o[0] = v.x; o[1] = v.y; o[2] = v.z; };
  setQ(d.qWorldFromDriverRotation, RandQ());
  setV(d.vecWorldFromDriverTranslation, RandV());
  setQ(d.qDriverFromHeadRotation, RandQ());
  setV(d.vecDriverFromHeadTranslation, RandV(0.1));
  setQ(d.qRotation, RandQ());
  setV(d.vecPosition, RandV());
  d.poseIsValid = true;
  return d;
}

// End to end: overlay builds Traw from the API poses, driver computes R from
// its H, poses get R applied; the HMD must end up at S_tgt in standing space.
static void TestRecenterPipeline() {
  for (int i = 0; i < 2000; ++i) {
    const vr::DriverPose_t d = RandDriverPose();
    const Rigid H = HeadFromDriverPose(d);  // driver-world head
    const Rigid Rold = (i % 3 == 0) ? Rigid{} : RandR();
    // Standing zero pose: raw-from-standing is yaw + translation.
    const Rigid Z{AxisAngle({0, 1, 0}, U(-3, 3)), {U(-2, 2), U(-0.5, 0.5), U(-2, 2)}};
    const Rigid P_cur = Compose(Rold, H);                 // RawAndUncalibrated
    const Rigid S_cur = Compose(Inverse(Z), P_cur);       // Standing

    RecenterOptions o;
    o.levelOnly = i % 2;
    o.keepPosition = (i / 2) % 2;
    o.eyeHeight = U(0.3, 1.8);
    const Rigid Traw = ComputeTraw(S_cur, P_cur, o);

    // Driver side.
    const Rigid Rnew = Compose(Traw, Inverse(H));
    vr::DriverPose_t p = d;
    ApplyWorldTransform(Rnew, &p);
    const Rigid rawNew = HeadFromDriverPose(p);
    CHECK(Near(rawNew, Compose(Rnew, H), 1e-9, 1e-7), "hook applies R to head pose");
    const Rigid S_new = Compose(Inverse(Z), rawNew);

    if (o.keepPosition)
      CHECK(Length(S_new.t - S_cur.t) < 1e-9, "keep position");
    else
      CHECK(Length(S_new.t - Vec3{0, o.eyeHeight, 0}) < 1e-9, "target height");
    if (o.levelOnly) {
      CHECK(Length(Rotate(S_new.q, {0, 1, 0}) - Vec3{0, 1, 0}) < 1e-7, "level-only up");
      CHECK(AngleBetween(S_new.q, LevelOnly(S_cur.q)) < 1e-7, "level-only heading");
    } else {
      CHECK(AngleBetween(S_new.q, Quat{}) < 1e-7, "full: identity orientation (%g rad)",
            AngleBetween(S_new.q, Quat{}));
    }
    // Head motion after recenter maps rigidly (no swim): moving the tracker by
    // Δ in driver space moves the raw head by the same Rnew-conjugated delta.
    vr::DriverPose_t d2 = d;
    d2.vecPosition[1] += 0.05;
    vr::DriverPose_t p2 = d2;
    ApplyWorldTransform(Rnew, &p2);
    CHECK(Near(HeadFromDriverPose(p2), Compose(Rnew, HeadFromDriverPose(d2)), 1e-9, 1e-7),
          "rigid after motion");
  }
}

// With a view axis below the head axis, the recentered *view* axis must be
// level and forward (Full) or level (Level only).
static void TestViewPitch() {
  // Steam Frame left eye: m11 0.6905, m12 -0.1846, lens centre at v 0.4983.
  const double vp = ViewPitchFromProjection(0.6905, -0.1846, 0.4983);
  CHECK(std::fabs(Deg(vp) + 14.7) < 0.2, "frame view pitch %.2f deg", Deg(vp));
  CHECK(std::fabs(ViewPitchFromProjection(0.7, 0.0, 0.5)) < 1e-12, "symmetric frustum");
  for (int i = 0; i < 1000; ++i) {
    const Rigid S_cur{RandQ(), RandV()};
    const Rigid Z{AxisAngle({0, 1, 0}, U(-3, 3)), RandV()};
    const Rigid P_cur = Compose(Z, S_cur);
    RecenterOptions o;
    o.levelOnly = i % 2;
    o.viewPitch = Rad(U(-20, 5));
    const Rigid Traw = ComputeTraw(S_cur, P_cur, o);
    const Rigid S_new = Compose(Inverse(Z), Traw);
    const Quat view = S_new.q * AxisAngle({1, 0, 0}, o.viewPitch);
    const Vec3 fwd = Rotate(view, {0, 0, -1}), up = Rotate(view, {0, 1, 0});
    CHECK(std::fabs(fwd.y) < 1e-9 && Length(up - Vec3{0, 1, 0}) < 1e-9, "view axis level");
    if (!o.levelOnly) CHECK(Length(fwd - Vec3{0, 0, -1}) < 1e-9, "view axis forward");
    else {
      const Quat oldView = S_cur.q * AxisAngle({1, 0, 0}, o.viewPitch);
      CHECK(AngleBetween(view, LevelOnly(oldView)) < 1e-9, "level-only keeps view heading");
    }
  }
}

static void TestDebugPitch() {
  // Debug pitch about the head position keeps the head position fixed.
  const Vec3 h{0.3, 1.1, -0.4};
  Rigid R;
  R.q = AxisAngle({1, 0, 0}, Rad(90));
  R.t = h - Rotate(R.q, h);
  CHECK(Length(Compose(R, Rigid{{}, h}).t - h) < 1e-12, "pitch about head");
}

static void TestSanity() {
  CHECK(IsSaneRigid(Rigid{}), "identity sane");
  CHECK(!IsSaneRigid(Rigid{{2, 0, 0, 0}, {}}), "non-unit q");
  CHECK(!IsSaneRigid(Rigid{{}, {NAN, 0, 0}}), "nan");
  CHECK(!IsSaneRigid(Rigid{{}, {1e6, 0, 0}}), "far");
  CHECK(IsIdentity(Rigid{}) && !IsIdentity(Rigid{{}, {0, 1e-6, 0}}), "identity");
}

int main() {
  TestBasics();
  TestLevelOnly();
  TestRecenterPipeline();
  TestViewPitch();
  TestDebugPitch();
  TestSanity();
  if (g_fail) {
    fprintf(stderr, "%d failure(s)\n", g_fail);
    return 1;
  }
  printf("test_math: all passed\n");
  return 0;
}
