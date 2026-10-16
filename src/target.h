#ifndef SRC_TARGET_H_
#define SRC_TARGET_H_

#include <godot_cpp/classes/character_body3d.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

namespace godot {

class Target : public CharacterBody3D {
  GDCLASS(Target, CharacterBody3D);

 public:
  Target();
  ~Target() override;

  void _ready() override;
  void _physics_process(double delta) override;

  void TakeDamage(float amount, const Vector3& from);
  void Respawn();

  // On a client, the server simulates the target, and the client applies
  // the position and death it sends.
  void set_simulated(bool simulated);
  void ApplyState(const Vector3& position, bool dead);

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
