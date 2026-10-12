// When to undo the recline on its own: SteamVR's boundary, proximity fade and
// passthrough bubble are only right without R, so R must not stay active once
// the user can walk around (see docs/DESIGN.md, "Automatic reset").
#pragma once

#include <cmath>
#include <cstdint>

#include "recline_math.h"
#include "recline_shm.h"

namespace recline {

class AutoResetGuard {
 public:
  // Measured on the Frame relative to the head position at recline time: lying
  // or reclining in a chair, shifting, reaching and rolling over stayed within
  // 0.23 m horizontally and 0.26 m up; sitting up or getting out of a chair
  // passed 0.4 m horizontally about a second in, before the first step.
  static constexpr double kMaxHorizontal = 0.40;  // m
  static constexpr double kMaxRise = 0.35;        // m
  static constexpr double kHold = 0.25;           // s over a limit before acting
  // The proximity sensor reports "not worn" for ~0.5 s (up to ~1 s) at random
  // while worn; a real removal stays that way.
  static constexpr double kRemovedHold = 3.0;  // s

  // `head`: driver-world head position (gravity-aligned, before R).
  void Arm(Vec3 head) {
    armed_ = true;
    anchor_ = head;
    overSince_ = -1;
  }
  void Disarm() { armed_ = false; }
  bool Armed() const { return armed_; }

  // `removedSince`: when the headset was last reported not worn, or < 0 while
  // worn. `standby`: the headset entered standby. Returns an AutoResetReason,
  // or AUTO_RESET_NONE.
  uint32_t Update(double now, bool haveHead, Vec3 head, double removedSince, bool standby) {
    if (!armed_) return AUTO_RESET_NONE;
    if (standby || (removedSince >= 0 && now - removedSince >= kRemovedHold)) return AUTO_RESET_REMOVED;
    if (!haveHead) return AUTO_RESET_NONE;
    const Vec3 d = head - anchor_;
    if (std::hypot(d.x, d.z) <= kMaxHorizontal && d.y <= kMaxRise) {
      overSince_ = -1;
      return AUTO_RESET_NONE;
    }
    if (overSince_ < 0) overSince_ = now;
    return now - overSince_ >= kHold ? AUTO_RESET_MOVED : AUTO_RESET_NONE;
  }

 private:
  bool armed_ = false;
  Vec3 anchor_{};
  double overSince_ = -1;
};

}  // namespace recline
