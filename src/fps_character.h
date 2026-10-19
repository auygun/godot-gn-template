#ifndef SRC_FPS_CHARACTER_H_
#define SRC_FPS_CHARACTER_H_

#include <cstdint>

#include <godot_cpp/classes/character_body3d.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

#include "src/net/entity.h"
#include "src/player_input.h"
#include "src/player_state.h"

namespace godot {

class Camera3D;
class Weapon;

// A player, replicated by the NetSession (src/net/): its owner's commands
// drive Simulate.
class FpsCharacter : public CharacterBody3D,
                     public net::TypedEntity<PlayerState, PlayerInput> {
  GDCLASS(FpsCharacter, CharacterBody3D);

 public:
  FpsCharacter();
  ~FpsCharacter() override;

  void _ready() override;
  void _physics_process(double delta) override;
  void _process(double delta) override;
  void _unhandled_input(const Ref<InputEvent>& event) override;

  // Named after its owner, and whether the player at this machine
  // controls this character (the default until spawned): other players'
  // characters drop their camera and touch controls, and take no local
  // input.
  void OnSpawning(net::PeerId owner, bool local) override;
  bool is_local() const { return local_; }

  // Advance the game state by one physics tick under the given command:
  // look, movement, recoil and the weapon. Reads nothing else from the
  // player, so a server can run the commands a client sends. A client
  // predicts its own player with it too (see net::SimulationMode); only
  // the server respawns a player that fell off the map.
  void Simulate(const PlayerInput& command,
                const net::SimContext& context) override;
  // The command from the local devices for this tick.
  PlayerInput GatherInput() override;
  // Until its owner's first command: standing still, looking where it
  // faces.
  PlayerInput IdleInput() const override;
  // Where shots come from: the eye, looking along the simulated aim plus
  // recoil. Game state only (no bob or interpolation), so a server without
  // a camera gets the same.
  Transform3D AimTransform() const;

  // The game state, for the server to send and a client to rewind to.
  PlayerState CaptureState() const override;
  // Set the game state, without effects.
  void RestoreState(const PlayerState& state) override;
  // Restore a state the server sent (or, for its own player, the first
  // one): the body moves there, a shot plays its effects, and a respawn
  // teleports.
  void ApplyState(const PlayerState& state) override;
  // After the game state jumped (a respawn, the first state): draw the
  // body there rather than sliding, and look where it faces.
  void OnTeleported() override;
  // The state the camera draws, for the player at this machine (see
  // PlayerState::Replicate): the position, recoil and landing dip between
  // ticks, with corrections easing out.
  void Draw(const PlayerState& view) override;

  // Back to the spawn transform (or the one given) with full health and
  // ammo and a still camera.
  void Respawn();
  void RespawnAt(const Transform3D& transform);
  // On the server: lose health from a hit, emitting "damaged". Returns
  // whether it killed. A dead player takes no damage.
  bool TakeDamage(int amount);
  // Apply a look delta in pixels (mouse or touch) to the local look. The
  // camera turns on the next frame; the next command carries it to the
  // simulation.
  void ApplyLookDelta(const Vector2& delta, float sensitivity);
  // Leave fire out of the commands until "shoot" is released (e.g. the
  // click that recaptured the mouse).
  void HoldFire();

  // View motion state, for the weapon's bob and sway. The step phase
  // advances one cycle per two steps; the bob amount is 0 when standing or
  // airborne, 1 at walk speed.
  float step_phase() const { return step_phase_; }
  float bob_amount() const { return bob_amount_; }
  // The landing dip as drawn this frame, in meters (negative is down).
  float landing_offset() const { return view_.landing_offset; }
  // The first-person camera; null on other players' characters.
  Camera3D* camera() const { return camera_; }
  Weapon* weapon() const { return weapon_; }
  uint8_t respawn_count() const { return state_.respawn_count; }
  // The view tick of the command being simulated (net::SimContext).
  double view_tick() const { return view_tick_; }
  int health() const { return state_.health; }
  // This match's score, kept by the server (Main) and sent with the state.
  int kills() const { return state_.kills; }
  int deaths() const { return state_.deaths; }
  void AddKill() { state_.kills++; }
  void AddDeath() { state_.deaths++; }
  bool dead() const { return state_.dead; }
  // On the floor after the last tick.
  bool on_floor() const { return state_.on_floor; }
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
  static constexpr int kMaxHealth = 100;
  // Eye height when the scene has no Camera3D to take it from (a server).
  static constexpr float kEyeHeight = 1.6f;  // meters
  // Each shot kicks the aim up (and pseudo-randomly sideways, the same on
  // every machine); exponential decay gives sustained fire a steady
  // offset.
  static constexpr float kRecoilKick = 0.03f;      // radians per shot
  static constexpr float kRecoilYawKick = 0.008f;  // max radians per shot
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
  // Place the (top-level) camera at the drawn body's eye (view_), looking
  // along the local look and the drawn recoil, moved by the bob and the
  // drawn landing dip.
  void UpdateCamera();
  void OnWeaponFired();
  // Show the body and let it collide (and be shot), or not (dead).
  void SetBodyActive(bool active);

  Camera3D* camera_ = nullptr;
  Weapon* weapon_ = nullptr;
  bool local_ = true;
  // The scene's floor snap length, used when on the floor.
  float floor_snap_length_ = 0.0f;
  // Whether ApplyState has run: the first state only places the character.
  bool has_state_ = false;
  // The game state, advanced only by Simulate. Its position and velocity
  // are the body's (the engine moves it): CaptureState and RestoreState
  // copy those, and state_ holds everything else. The weapon's state is
  // the weapon's.
  PlayerState state_;
  double view_tick_ = 0.0;
  // The eye's position relative to the body, from the scene's camera.
  Vector3 eye_offset_ = Vector3(0.0f, kEyeHeight, 0.0f);

  // Local state (the player at this machine), not part of the simulation.
  // The look as input arrives: the camera shows it every frame, and the
  // next command carries it.
  float look_yaw_ = 0.0f;
  float look_pitch_ = 0.0f;
  bool fire_held_ = false;
  float step_phase_ = 0.0f;
  float bob_amount_ = 0.0f;
  // What the camera draws this frame (Draw).
  PlayerState view_;
  // Look rotation applied since the last frame, in radians.
  Vector2 look_delta_;
  Vector2 look_velocity_;
  Vector3 local_velocity_;
  Transform3D spawn_transform_;
};

}  // namespace godot

#endif  // SRC_FPS_CHARACTER_H_
