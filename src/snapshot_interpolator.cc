#include "src/snapshot_interpolator.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <godot_cpp/core/math.hpp>

namespace godot {

namespace {

const SnapshotPlayer* FindPlayer(const SnapshotMessage& snapshot,
                                 int32_t peer_id) {
  for (const SnapshotPlayer& player : snapshot.players) {
    if (player.peer_id == peer_id) {
      return &player;
    }
  }
  return nullptr;
}

// The older state, moved a fraction t of the way to the newer one.
// Respawns and other jumps don't blend.
PlayerState Blend(const PlayerState& from, const PlayerState& to, float t) {
  PlayerState state = from;
  if (from.respawn_count != to.respawn_count) {
    return state;
  }
  state.position = from.position.lerp(to.position, t);
  state.velocity = from.velocity.lerp(to.velocity, t);
  state.yaw = Math::lerp_angle(from.yaw, to.yaw, t);
  state.pitch = Math::lerp(from.pitch, to.pitch, t);
  state.recoil = Math::lerp(from.recoil, to.recoil, t);
  state.recoil_yaw = Math::lerp(from.recoil_yaw, to.recoil_yaw, t);
  state.landing_offset = Math::lerp(from.landing_offset, to.landing_offset, t);
  return state;
}

}  // namespace

SnapshotInterpolator::SnapshotInterpolator() = default;

SnapshotInterpolator::~SnapshotInterpolator() = default;

void SnapshotInterpolator::Add(const SnapshotMessage& snapshot) {
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

bool SnapshotInterpolator::Sample(SnapshotMessage* out) {
  if (snapshots_.empty()) {
    return false;
  }
  auto time_of = [this](const SnapshotMessage& snapshot) {
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
  const SnapshotMessage& from = snapshots_.front();
  *out = from;
  if (snapshots_.size() < 2 || time_of(from) > render_time_) {
    // Past the newest (late packets) or before the oldest: hold.
    return true;
  }
  const SnapshotMessage& to = snapshots_[1];
  float t = (render_time_ - time_of(from)) / (time_of(to) - time_of(from));

  for (SnapshotPlayer& player : out->players) {
    const SnapshotPlayer* next = FindPlayer(to, player.peer_id);
    if (next) {
      player.state = Blend(player.state, next->state, t);
    }
  }
  size_t count = std::min(out->targets.size(), to.targets.size());
  for (size_t i = 0; i < count; i++) {
    SnapshotTarget& target_state = out->targets[i];
    if (target_state.dead == to.targets[i].dead) {
      target_state.position =
          target_state.position.lerp(to.targets[i].position, t);
    }
  }
  return true;
}

}  // namespace godot
