// Microbenchmark of the per-call work driver_recline's hook adds:
// bounded seqlock read of R, world transform of the pose, HMD try-lock publish.
#include <cstdio>
#include <ctime>

#include "../driver/pose_math.h"
#include "recline_shm.h"

using namespace recline;

alignas(64) static StateBlock g_r{};  // stands in for the in-process R copy
alignas(64) static HmdBlock g_hmd{};
static volatile double g_sink;

int main() {
  StateSnapshot init;
  const Quat q0 = AxisAngle({1, 0, 0}, 1.2);
  init.R_q[0] = q0.w; init.R_q[1] = q0.x; init.R_q[2] = q0.y; init.R_q[3] = q0.z;
  init.R_t[1] = 0.4;
  WriteState(g_r, init);
  vr::DriverPose_t in{};
  in.qWorldFromDriverRotation = {1, 0, 0, 0};
  in.qDriverFromHeadRotation = {1, 0, 0, 0};
  in.qRotation = {0.9, 0.1, 0.3, 0.2};
  in.poseIsValid = true;

  constexpr int kN = 2000000;
  const double t0 = MonotonicSeconds();
  uint64_t count = 0;
  for (int i = 0; i < kN; ++i) {
    in.vecPosition[0] = i * 1e-7;
    // HMD publish (every call here; in the driver only for the HMD)
    uint32_t s;
    if (SeqTryLock(g_hmd.seq, &s)) {
      const Rigid h = HeadFromDriverPose(in);
      St(g_hmd.q[0], h.q.w); St(g_hmd.q[1], h.q.x); St(g_hmd.q[2], h.q.y); St(g_hmd.q[3], h.q.z);
      St(g_hmd.t[0], h.t.x); St(g_hmd.t[1], h.t.y); St(g_hmd.t[2], h.t.z);
      St(g_hmd.time, MonotonicSeconds());
      St(g_hmd.pose_count, ++count);
      SeqWriteEnd(g_hmd.seq, s);
    }
    StateSnapshot r;
    ReadState(g_r, &r, 16);
    const Rigid R{{r.R_q[0], r.R_q[1], r.R_q[2], r.R_q[3]}, {r.R_t[0], r.R_t[1], r.R_t[2]}};
    if (!IsSaneRigid(R)) return 1;
    vr::DriverPose_t p = in;
    ApplyWorldTransform(R, &p);
    g_sink = p.vecWorldFromDriverTranslation[1];
  }
  const double dt = MonotonicSeconds() - t0;
  printf("hook work: %.1f ns/call (HMD path incl. publish + clock), %d calls\n", dt / kN * 1e9, kN);
  return 0;
}
