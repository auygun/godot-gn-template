#ifndef SRC_NET_SNAPSHOT_INTERPOLATOR_H_
#define SRC_NET_SNAPSHOT_INTERPOLATOR_H_

#include <cstdint>
#include <deque>

#include "src/net/protocol.h"

namespace godot::net {

// What a client shows of everything but its own entities: the server's
// snapshots, drawn a little in the past so there is always one on each
// side of the moment drawn, and blended between them. Packets arriving
// unevenly or lost (up to the delay's worth) then don't show.
//
// A render clock counts server ticks: one per tick, and nudged towards
// kDelayTicks behind the server's (estimated) time, or jumped there when
// far off.
class SnapshotInterpolator {
 public:
  SnapshotInterpolator();
  ~SnapshotInterpolator();

  // A snapshot arrived (any order).
  void Add(const Snapshot& snapshot);
  // Advance the render clock one tick, and give the state to draw at it:
  // the kBlend fields blended between the snapshots around it, the rest
  // from the older one. False until there is a snapshot.
  bool Sample(Snapshot* out);
  // The server tick drawn (fractional), as of the last Sample; 0 before
  // the first.
  double render_tick() const {
    return started_ ? static_cast<double>(base_tick_) + render_time_ : 0.0;
  }

 private:
  // How far in the past to draw: 100 ms at 60 ticks, three snapshots at
  // 30 per second, so one lost snapshot still leaves one on each side.
  static constexpr float kDelayTicks = 6.0f;
  // How much faster or slower the clock runs to close the gap to its
  // target, at most; a gap under kDeadZone ticks (packets arriving
  // unevenly) is left alone.
  static constexpr float kMaxDrift = 0.05f;
  static constexpr float kDeadZone = 1.0f;
  // A gap past this (a stall, the first snapshots) is jumped, in ticks.
  static constexpr float kMaxGap = 30.0f;
  // Snapshots kept: a second at 30 per second.
  static constexpr size_t kMaxSnapshots = 32;

  // Oldest first, by tick.
  std::deque<Snapshot> snapshots_;
  // The newest tick received, and ticks sampled since: the server's time
  // is about their sum (it rises steadily between snapshots).
  uint32_t newest_tick_ = 0;
  int ticks_since_newest_ = 0;
  // In server ticks (relative to the first snapshot, to keep float
  // precision).
  float render_time_ = 0.0f;
  uint32_t base_tick_ = 0;
  bool started_ = false;
};

}  // namespace godot::net

#endif  // SRC_NET_SNAPSHOT_INTERPOLATOR_H_
