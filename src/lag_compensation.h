#ifndef SRC_LAG_COMPENSATION_H_
#define SRC_LAG_COMPENSATION_H_

#include <array>
#include <cstdint>
#include <vector>

#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

class Node3D;

// On the server: where the shootable bodies (players and targets) were on
// each recent tick, so a shot can be tested against what its shooter saw.
// A client draws them ~100 ms in the past (SnapshotInterpolator), so
// against the bodies' current positions a shot that was on target on the
// shooter's screen would miss.
//
// Moving the bodies back for the test doesn't work: the physics space only
// sees a moved body after the next physics step. So the history keeps each
// body's hitbox (a capsule or a box, from its CollisionShape3D), and shots
// are tested against them here, in plain math.
class LagCompensation {
 public:
  ~LagCompensation();

  // Bodies in this group (set in their scenes) are tested in the past:
  // Main records every network entity in it.
  static constexpr const char* kGroup = "lag_compensated";

  // shot_mask: the collision layers shots hit (Weapon::kShotMask).
  explicit LagCompensation(uint32_t shot_mask);

  // Record the bodies' hitboxes for a tick (after it was simulated). A
  // body that can't be hit now isn't recorded: one off the shot layers, or
  // whose shape is disabled (e.g. dead).
  void Record(uint32_t tick, const std::vector<Node3D*>& bodies);
  // Whether a body can be hit now (as Record decides).
  bool Hittable(const Node3D* body) const;

  // The nearest body the ray from origin along direction (normalized), at
  // most max_distance long, hits as the bodies were at view_tick (between
  // recorded ticks, they're blended); never ignore. Null if none. Older
  // than the history goes, it tests the oldest kept.
  Node3D* Raycast(float view_tick,
                  const Vector3& origin,
                  const Vector3& direction,
                  float max_distance,
                  const Node3D* ignore,
                  Vector3* hit_position,
                  Vector3* hit_normal) const;

  // The oldest tick kept.
  uint32_t oldest_tick() const;

 private:
  // A second at 60 ticks.
  static constexpr uint32_t kHistoryTicks = 64;

  struct Hitbox {
    Node3D* node = nullptr;
    // The shape's transform (its center), and its size: a capsule's radius
    // and total height, or a box's half extents.
    Transform3D transform;
    bool capsule = false;
    float radius = 0.0f;
    float height = 0.0f;
    Vector3 half_extents;
  };
  struct Frame {
    uint32_t tick = 0;
    bool valid = false;
    std::vector<Hitbox> hitboxes;
  };

  const Frame* FindFrame(uint32_t tick) const;

  uint32_t shot_mask_;
  std::array<Frame, kHistoryTicks> frames_;
  uint32_t newest_tick_ = 0;
};

}  // namespace godot

#endif  // SRC_LAG_COMPENSATION_H_
