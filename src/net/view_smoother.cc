#include "src/net/view_smoother.h"

#include <algorithm>

namespace godot::net {

ViewSmoother::ViewSmoother() = default;

ViewSmoother::~ViewSmoother() = default;

void ViewSmoother::Reset() {
  has_state_ = false;
  corrections_.clear();
}

void ViewSmoother::Tick(const Box& state, float dt) {
  dt_ = dt;
  ticks_++;
  if (!has_state_ || state.layout() != cur_.layout() ||
      !SameTeleportFields(cur_, state)) {
    // The first state, or a teleport (e.g. a respawn): drawn there, not
    // sliding from where it was.
    has_state_ = true;
    prev_ = state;
    cur_ = state;
    corrections_.clear();
    return;
  }
  prev_ = cur_;
  cur_ = state;
  std::erase_if(corrections_, [this](const Correction& correction) {
    float age = Age(correction, 0.0f);
    return age > kMaxCorrectionAge || !StillEasing(*cur_.layout(), age);
  });
}

void ViewSmoother::Corrected(const Box* prev, const Box& cur) {
  if (!has_state_ || cur.layout() != cur_.layout()) {
    Teleported(cur);
    return;
  }
  Box corrected_prev = prev ? *prev : prev_;
  // At the end of the last tick's interval (fraction 1), where the next
  // tick's starts: what was drawn, and what would be now.
  Box predicted;
  Box corrected;
  DrawFields(prev_, cur_, 1.0f, dt_, &predicted);
  DrawFields(corrected_prev, cur, 1.0f, dt_, &corrected);
  Correction correction;
  SubtractEased(predicted, corrected, &correction.diff);
  correction.tick = ticks_;
  prev_ = corrected_prev;
  cur_ = cur;

  // Too far to ease, with what is left of the earlier ones: snap there.
  Box offset = correction.diff;
  for (const Correction& each : corrections_) {
    AddEased(each.diff, Age(each, 1.0f), &offset);
  }
  if (ExceedsSnap(offset)) {
    corrections_.clear();
    return;
  }
  corrections_.push_back(correction);
}

void ViewSmoother::Teleported(const Box& cur) {
  has_state_ = true;
  prev_ = cur;
  cur_ = cur;
  corrections_.clear();
}

void ViewSmoother::View(float fraction, Box* out) const {
  DrawFields(prev_, cur_, fraction, dt_, out);
  for (const Correction& correction : corrections_) {
    AddEased(correction.diff, Age(correction, fraction), out);
  }
}

void ViewSmoother::Latest(Box* out) const {
  *out = cur_;
  for (const Correction& correction : corrections_) {
    AddEased(correction.diff, Age(correction, 0.0f), out);
  }
}

float ViewSmoother::Age(const Correction& correction, float fraction) const {
  // A correction is made between ticks, at the end of one tick's interval:
  // 0 at the start of the next's.
  float ticks = static_cast<float>(ticks_ - correction.tick) - 1.0f + fraction;
  return std::max(ticks, 0.0f) * dt_;
}

}  // namespace godot::net
