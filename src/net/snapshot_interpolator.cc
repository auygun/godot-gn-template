#include "src/net/snapshot_interpolator.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace godot::net {

SnapshotInterpolator::SnapshotInterpolator() = default;

SnapshotInterpolator::~SnapshotInterpolator() = default;

void SnapshotInterpolator::Add(const Snapshot& snapshot) {
  if (!started_ && snapshots_.empty()) {
    base_tick_ = snapshot.tick;
  }
  if (snapshots_.empty() || SequenceNewer(snapshot.tick, newest_tick_)) {
    newest_tick_ = snapshot.tick;
    ticks_since_newest_ = 0;
  }
  // Too late to draw: the render time is already past it.
  if (started_ && static_cast<float>(static_cast<int32_t>(
                      snapshot.tick - base_tick_)) <= render_time_) {
    return;
  }
  // In order.
  auto it = snapshots_.end();
  while (it != snapshots_.begin() &&
         SequenceNewer(std::prev(it)->tick, snapshot.tick)) {
    --it;
  }
  if (it != snapshots_.begin() && std::prev(it)->tick == snapshot.tick) {
    return;
  }
  snapshots_.insert(it, snapshot);
  if (snapshots_.size() > kMaxSnapshots) {
    snapshots_.pop_front();
  }
}

bool SnapshotInterpolator::Sample(Snapshot* out) {
  if (snapshots_.empty()) {
    return false;
  }
  auto time_of = [this](const Snapshot& snapshot) {
    return static_cast<float>(static_cast<int32_t>(snapshot.tick - base_tick_));
  };

  float server_time =
      static_cast<float>(static_cast<int32_t>(newest_tick_ - base_tick_)) +
      static_cast<float>(ticks_since_newest_++);
  float target = server_time - kDelayTicks;
  float gap = target - render_time_;
  if (!started_ || std::abs(gap) > kMaxGap) {
    render_time_ = target;
    started_ = true;
  } else {
    // Close the gap gradually: a step in the drawn motion would show.
    float drift = 0.0f;
    if (std::abs(gap) > kDeadZone) {
      drift = std::clamp(gap * 0.1f, -kMaxDrift, kMaxDrift);
    }
    render_time_ += 1.0f + drift;
  }

  // The last snapshot at or before the render time, and the one after.
  // Older ones aren't needed again.
  while (snapshots_.size() > 1 && time_of(snapshots_[1]) <= render_time_) {
    snapshots_.pop_front();
  }
  const Snapshot& from = snapshots_.front();
  *out = from;
  if (snapshots_.size() < 2 || time_of(from) > render_time_) {
    // Past the newest (late packets) or before the oldest: hold.
    return true;
  }
  const Snapshot& to = snapshots_[1];
  float t = (render_time_ - time_of(from)) / (time_of(to) - time_of(from));

  for (SnapshotEntity& entity : out->entities) {
    const SnapshotEntity* next = to.Find(entity.id);
    if (next && next->type == entity.type && next->owner == entity.owner) {
      Box blended;
      Blend(entity.state, next->state, t, &blended);
      entity.state = blended;
    }
  }
  return true;
}

}  // namespace godot::net
