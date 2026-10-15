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

  // View motion state, for the weapon's bob and sway. The step phase
  // advances one cycle per two steps; the bob amount is 0 when standing or
  // airborne, 1 at walk speed.
  float step_phase() const { return step_phase_; }
  float bob_amount() const { return bob_amount_; }
  // The landing dip as drawn this frame, in meters (negative is down).
  float landing_offset() const;
  // Look turn rate (x: yaw, y: pitch), in radians per second.
  Vector2 look_velocity() const { return look_velocity_; }
  // Actual velocity in the character's local space.
  Vector3 local_velocity() const { return local_velocity_; }

 protected:
  static void _bind_methods();

 private:
  static constexpr float kGravity = 20.0f;
  static constexpr float kWalkSpeed = 5.0f;
  static constexpr float kSprintSpeed = 9.0f;
  static constexpr float kJumpVelocity = 7.0f;
  // How fast the horizontal velocity reaches the input's, in m/s^2: full
  // walk speed in ~0.08 s on the ground, limited steering in the air.
  static constexpr float kGroundAccel = 60.0f;
  static constexpr float kAirAccel = 15.0f;
  static constexpr float kMouseSensitivity = 0.0022f;
  static constexpr float kPitchLimit = 1.55f;  // ~89 degrees
  // Below this height the player has fallen off the map and respawns.
  static constexpr float kFallLimit = -20.0f;
  // Each shot kicks the view up (and randomly sideways); exponential decay
  // gives sustained fire a steady offset.
  static constexpr float kRecoilKick = 0.03f;      // radians per shot
  static constexpr float kRecoilYawKick = 0.008f;  // max radians per shot
  static constexpr float kRecoilReturn = 6.0f;     // decay rate, 1/s
  // Head bob: step cycles (two steps each) per second at walk speed. It
  // grows with the square root of speed (longer strides when sprinting).
  static constexpr float kStepRate = 1.5f;
  // Bob size at walk speed: the head dips on each step. Vertical only:
  // tilting the horizon (roll) is a common cause of motion sickness, and
  // moving sideways would move the crosshair off the line shots take. The
  // weapon's bob and sway carry most of the walking feel.
  static constexpr float kBobUp = 0.018f;  // meters
  // Bob amount cap (sprinting bobs this much more than walking) and how
  // fast the bob fades in and out, 1/s.
  static constexpr float kMaxBobAmount = 1.5f;
  static constexpr float kBobBlendRate = 8.0f;
  // Landing dips the eye on a critically damped spring: the impact speed
  // times kLandingImpulse kicks the spring's velocity. A jump lands at
  // ~7 m/s, a ~3 cm dip. Slower impacts (stepping off a ledge, floor
  // jitter) are ignored, faster ones capped.
  static constexpr float kLandingImpulse = 0.18f;
  static constexpr float kMinLandingSpeed = 2.0f;   // m/s
  static constexpr float kMaxLandingSpeed = 12.0f;  // m/s
  static constexpr float kLandingSpring = 16.0f;    // angular frequency, 1/s

  // On ticks, with the body: advance the landing dip's spring.
  // landing_speed is the downward speed of a landing this tick, 0 if none.
  void UpdateLandingDip(float dt, float landing_speed);
  // Every frame, so it is smooth at any refresh rate: advance the bob.
  void UpdateBob(float dt);
  // Place the (top-level) camera at the body's eye, looking along yaw_,
  // pitch_ and the recoil (decayed by since_tick seconds past the last
  // tick), moved by the bob and landing_dip.
  void UpdateCamera(const Vector3& origin, float landing_dip, float since_tick);
  void OnWeaponFired();

  Camera3D* camera_ = nullptr;
  // The camera's position relative to the body, from the scene.
  Vector3 camera_position_;
  Weapon* weapon_ = nullptr;
  float yaw_ = 0.0f;
  float pitch_ = 0.0f;
  float recoil_ = 0.0f;
  float recoil_yaw_ = 0.0f;
  // The landing dip's spring, and its offset before the last tick (for
  // drawing between ticks).
  float landing_offset_ = 0.0f;
  float landing_velocity_ = 0.0f;
  float prev_landing_offset_ = 0.0f;
  float step_phase_ = 0.0f;
  float bob_amount_ = 0.0f;
  // Look rotation applied since the last frame, in radians.
  Vector2 look_delta_;
  Vector2 look_velocity_;
  Vector3 local_velocity_;
  Transform3D spawn_transform_;
};

}  // namespace godot

#endif  // SRC_FPS_CHARACTER_H_
