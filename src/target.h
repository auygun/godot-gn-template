#ifndef SRC_TARGET_H_
#define SRC_TARGET_H_

#include <godot_cpp/classes/character_body3d.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

#include "src/net/entity.h"
#include "src/player_state.h"

namespace godot {

// A practice target, replicated by the NetSession: nobody commands it, the
// server simulates it on its own and clients draw its state.
class Target : public CharacterBody3D, public net::TypedEntity<TargetState> {
  GDCLASS(Target, CharacterBody3D);

 public:
  Target();
  ~Target() override;

  void _ready() override;
  void _physics_process(double delta) override;

  void TakeDamage(float amount, const Vector3& from);
  void Respawn();

  // On a client: the server simulates the target, and the client applies
  // the state it sends.
  void OnRemoteAuthority() override;
  TargetState CaptureState() const override;
  void ApplyState(const TargetState& state) override;

  float health() const { return health_; }
  bool dead() const { return dead_; }

 protected:
  static void _bind_methods();

 private:
  static constexpr float kGravity = 20.0f;
  static constexpr float kRespawnTime = 3.0f;
  // Knockback slowdown on the floor, m/s^2.
  static constexpr float kFriction = 8.0f;

  float health_ = 100.0f;
  float max_health_ = 100.0f;
  bool dead_ = false;
  float respawn_timer_ = 0.0f;
  Transform3D spawn_transform_;

  // Collision off while dead (it falls through the floor), and back on,
  // without the bullet holes, on respawn.
  void SetDead(bool dead);
};

}  // namespace godot

#endif  // SRC_TARGET_H_
