#ifndef SRC_FPS_CHARACTER_H_
#define SRC_FPS_CHARACTER_H_

#include <godot_cpp/classes/character_body3d.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

namespace godot {

class Camera3D;
class Weapon;

class FpsCharacter : public CharacterBody3D {
  GDCLASS(FpsCharacter, CharacterBody3D);

 public:
  FpsCharacter();
  ~FpsCharacter() override;

  void _ready() override;
  void _physics_process(double delta) override;
  void _process(double delta) override;
  void _unhandled_input(const Ref<InputEvent>& event) override;

  // Back to the spawn transform with full ammo and a still camera.
  void Respawn();
  // Apply a look delta in pixels (mouse or touch). The camera turns on the
  // next frame, the body on the next tick.
  void ApplyLookDelta(const Vector2& delta, float sensitivity);

 protected:
  static void _bind_methods();

 private:
  static constexpr float kGravity = 20.0f;
  static constexpr float kWalkSpeed = 5.0f;
  static constexpr float kSprintSpeed = 9.0f;
  static constexpr float kJumpVelocity = 7.0f;
  static constexpr float kMouseSensitivity = 0.0022f;
  static constexpr float kPitchLimit = 1.55f;  // ~89 degrees
  // Below this height the player has fallen off the map and respawns.
  static constexpr float kFallLimit = -20.0f;
  // Each shot kicks the view up (and randomly sideways); exponential decay
  // gives sustained fire a steady offset.
  static constexpr float kRecoilKick = 0.03f;      // radians per shot
  static constexpr float kRecoilYawKick = 0.008f;  // max radians per shot
  static constexpr float kRecoilReturn = 6.0f;     // decay rate, 1/s

  // Place the (top-level) camera at the body's eye, looking along yaw_,
  // pitch_ and the recoil, decayed by since_tick seconds past the last
  // tick.
  void UpdateCamera(const Vector3& origin, float since_tick);
  void OnWeaponFired();

  Camera3D* camera_ = nullptr;
  // The camera's position relative to the body, from the scene.
  Vector3 camera_position_;
  Weapon* weapon_ = nullptr;
  float yaw_ = 0.0f;
  float pitch_ = 0.0f;
  float recoil_ = 0.0f;
  float recoil_yaw_ = 0.0f;
  Transform3D spawn_transform_;
};

}  // namespace godot

#endif  // SRC_FPS_CHARACTER_H_
