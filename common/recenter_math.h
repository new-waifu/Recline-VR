// Recenter target math (SPEC §5.3), independent of the OpenVR client API so
// it can be unit-tested together with the driver-side pose math.
#pragma once

#include "recline_math.h"

namespace recline {

struct RecenterOptions {
  bool levelOnly = false;     // rotation: Full (false) | Level only (true)
  bool keepPosition = false;  // position: Target height (false) | Keep position (true)
  double eyeHeight = 1.20;    // m, for Target height
};

// Pure math part of §5.3, separated for unit tests.
//   S_cur: HMD in TrackingUniverseStanding
//   P_cur: HMD in TrackingUniverseRawAndUncalibrated
// Returns Traw = Z ∘ S_tgt with Z = P_cur ∘ S_cur⁻¹ (raw-from-standing).
inline Rigid ComputeTraw(const Rigid& S_cur, const Rigid& P_cur, const RecenterOptions& o) {
  const Rigid Z = Compose(P_cur, Inverse(S_cur));
  Rigid S_tgt;
  S_tgt.q = o.levelOnly ? LevelOnly(S_cur.q) : Quat{};
  S_tgt.t = o.keepPosition ? S_cur.t : Vec3{0, o.eyeHeight, 0};
  return Compose(Z, S_tgt);
}

}  // namespace recline
