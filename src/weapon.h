#ifndef SRC_WEAPON_H_
#define SRC_WEAPON_H_

#include <array>
#include <cstdint>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "src/player_state.h"

namespace godot {

class AudioStreamPlayer3D;
class Camera3D;
class CPUParticles3D;
class GPUParticles3D;
class OmniLight3D;

// Decides what a shot hits among the shootable bodies (players and
// targets), when the physics space can't: the server tests them as the
// shooter saw them (LagCompensation).
class ShotResolver {
 public:
  // The bodies it decides about, for the physics ray to pass through.
  virtual TypedArray<RID> ShootableBodies() = 0;
  // The nearest body the ray hits within max_distance, as seen at
  // view_tick, never shooter; null if none.
  virtual Node3D* ResolveShot(const Node3D* shooter,
                              double view_tick,
                              const Vector3& origin,
                              const Vector3& direction,
                              float max_distance,
                              Vector3* position,
                              Vector3* normal) = 0;

 protected:
  virtual ~ShotResolver() = default;
};

class Weapon : public Node3D {
  GDCLASS(Weapon, Node3D);

 public:
  // Physics layers shots hit (named in project.godot): world (1),
  // shot_only (3) and player (4), not player_clip (2). With a
  // ShotResolver, the ray passes through its bodies.
  static constexpr uint32_t kShotMask = (1u << 0) | (1u << 2) | (1u << 3);

  Weapon();
  ~Weapon() override;

  void _ready() override;
  void _process(double delta) override;
  void _physics_process(double delta) override;

  // One physics tick, from the owner's simulation: advance reload and fire
  // timers, then fire (while fire is held) or start reloading (on a reload
  // press).
  void Tick(float dt,
            bool fire,
            bool reload_pressed,
            net::SimulationMode mode = net::SimulationMode::kAuthoritative);
  void Shoot();
  void StartReload();
  // Full magazine and reserve, reload cancelled (respawn).
  void Refill();

  // The camera to follow and aim from (the owner's first-person camera);
  // null leaves the weapon where the scene put it, on the owner's body.
  void SetCamera(Camera3D* camera);
  // Where an authoritative shot's hits on players and targets come from;
  // without one, the physics space's ray decides.
  void SetShotResolver(ShotResolver* resolver) { shot_resolver_ = resolver; }

  // The game state, for the owner's.
  WeaponState CaptureState() const;
  void RestoreState(const WeaponState& state);
  // A shot from aim that the server simulated: its effects, and an impact
  // where a ray (cosmetic, against what this machine sees) hits.
  void PlayShotEffects(const Transform3D& aim);

  int ammo() const { return state_.ammo; }
  int reserve_ammo() const { return state_.reserve_ammo; }

  // Instanced where a shot hits; see Impact.
  void set_impact_scene(const Ref<PackedScene>& scene) {
    impact_scene_ = scene;
  }
  Ref<PackedScene> impact_scene() const { return impact_scene_; }

 protected:
  static void _bind_methods();

 private:
  // Length of the hitscan ray, in meters.
  // The magazine, and the spare ammo a (re)spawn starts with.
  static constexpr int kMaxAmmo = 30;
  static constexpr int kStartReserveAmmo = 300;
  static constexpr float kRange = 100.0f;
  // The barrel aims at the crosshair's hit point, but never closer than
  // this (pressed against a wall it would turn sideways), in meters.
  static constexpr float kMinAimDistance = 2.0f;
  // How fast the aim distance follows the crosshair's hit point, 1/s.
  static constexpr float kAimDistanceRate = 15.0f;
  // Each shot kicks the weapon back and its muzzle up (and randomly
  // sideways), on top of the camera's recoil; decays back exponentially.
  static constexpr float kKickBack = 0.04f;    // meters per shot
  static constexpr float kKickPitch = 0.07f;   // radians per shot
  static constexpr float kKickYaw = 0.015f;    // max radians per shot
  static constexpr float kKickReturn = 12.0f;  // decay rate, 1/s
  // The kick rotates around the grip, this far behind the weapon's origin.
  static constexpr float kGripOffset = 0.2f;  // meters
  // Walking bobs the weapon in a figure eight on top of the camera's bob,
  // with a sideways rock; sizes at walk speed.
  static constexpr float kBobSide = 0.012f;  // meters
  static constexpr float kBobUp = 0.008f;    // meters
  static constexpr float kBobRoll = 0.02f;   // radians
  // The weapon lags behind turning (radians per radian/s of turn rate,
  // capped) and behind moving (meters per m/s, capped), springing back at
  // kSwayRate, 1/s.
  static constexpr float kLookSway = 0.012f;
  static constexpr float kMaxLookSway = 0.08f;  // radians
  static constexpr float kMoveSway = 0.004f;
  static constexpr float kMaxMoveSway = 0.03f;  // meters
  static constexpr float kSwayRate = 10.0f;
  // Landing dips the weapon this much more than the camera.
  static constexpr float kLandingDip = 0.6f;
  // How long the muzzle flash stays up after a shot, in seconds.
  static constexpr float kFlashTime = 0.06f;
  static constexpr float kFlashLightEnergy = 3.0f;
  // How long the smoke emitter runs after a shot, in seconds. Sustained
  // fire keeps it running, so the smoke thickens.
  static constexpr float kSmokeTime = 0.1f;
  // At most this many impacts stay alive; a new one frees the oldest.
  static constexpr int kMaxImpacts = 32;
  // The Mobile renderer applies only 8 decals per mesh (more flicker), and
  // the floor is one mesh.
  static constexpr int kMaxImpactsMobile = 8;

  // Where shots come from: the owner's aim (game state, the same on a
  // server), or the weapon's own transform without an owner.
  Transform3D ShotTransform() const;
  // Ray from aim's origin along its -Z, kRange long, not hitting the
  // owner nor the excluded bodies. Returns intersect_ray's result (empty:
  // no hit).
  Dictionary CastRay(const Transform3D& aim,
                     const TypedArray<RID>& exclude = TypedArray<RID>());
  // An authoritative shot's hit: the nearest of the level (the physics
  // ray) and, with a ShotResolver, its bodies. Sets the collider (null: a
  // miss), position and normal.
  Object* ResolveHit(const Transform3D& aim,
                     Vector3* position,
                     Vector3* normal);
  // Measure how far the crosshair's hit point is (a physics query, so once
  // per tick), for UpdatePose to aim the barrel at.
  void UpdateAimTarget();
  // Follow the camera (keeping the scene's offset from it) and point the
  // barrel at what the crosshair is on.
  void UpdatePose(float dt);
  // Fire a shot from the owner's aim. Only an authoritative shot hits;
  // a predicted one plays its effects, a replayed one nothing.
  void Fire(net::SimulationMode mode);
  // A shot's kick, muzzle flash and sound.
  void PlayFireEffects();
  void ShowMuzzleFlash();
  void PlayFireSound();
  // Leave an impact on the collider at the hit point, facing along the
  // surface normal.
  void SpawnImpact(Object* collider,
                   const Vector3& position,
                   const Vector3& normal);

  Camera3D* camera_ = nullptr;
  // The weapon's transform relative to the camera, from the scene.
  Transform3D camera_offset_;
  float aim_target_distance_ = kRange;
  float aim_distance_ = kRange;
  float kick_back_ = 0.0f;
  float kick_pitch_ = 0.0f;
  float kick_yaw_ = 0.0f;
  // Smoothed sway: x yaw, y pitch (radians); position in camera space.
  Vector2 look_sway_;
  Vector3 move_sway_;

  // Muzzle effect nodes (Muzzle/Flash, its Light, Muzzle/Sparks,
  // Muzzle/Smoke).
  Node3D* flash_ = nullptr;
  OmniLight3D* flash_light_ = nullptr;
  CPUParticles3D* sparks_ = nullptr;
  GPUParticles3D* smoke_ = nullptr;
  float flash_timer_ = 0.0f;
  float smoke_timer_ = 0.0f;
  Vector3 flash_scale_ = Vector3(1.0f, 1.0f, 1.0f);
  // Muzzle/FireSound.
  AudioStreamPlayer3D* fire_sound_ = nullptr;

  ShotResolver* shot_resolver_ = nullptr;
  Ref<PackedScene> impact_scene_;
  // Instance IDs of the live impacts, a ring buffer of impact_limit_
  // entries; stale IDs (impacts that freed themselves) resolve to null.
  std::array<uint64_t, kMaxImpacts> impact_ids_ = {};
  int impact_limit_ = kMaxImpacts;
  int next_impact_ = 0;

  // The game state, advanced only by Tick (CaptureState copies it).
  WeaponState state_ = {.ammo = kMaxAmmo, .reserve_ammo = kStartReserveAmmo};

  float damage_ = 25.0f;
  float fire_rate_ = 8.0f;  // shots per second
  float reload_time_ = 1.5f;
};

}  // namespace godot

#endif  // SRC_WEAPON_H_
