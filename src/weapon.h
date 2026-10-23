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

class AnimationPlayer;
class AudioStreamPlayer3D;
class CPUParticles3D;
class FpsCharacter;
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

// The weapon of the FpsCharacter it is a child of: fires from its aim.
// Its model (Model, an instanced glb with an AnimationPlayer) stays where
// the scene put it, on the owner's body, unless a ViewModel holds it.
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

  // One physics tick, from the owner's simulation: advance reload and fire
  // timers, check for a wall in the barrel's way, then fire (while fire is
  // held and the weapon isn't tucked) or start reloading (on a reload
  // press).
  void Tick(float dt,
            bool fire,
            bool reload_pressed,
            net::SimulationMode mode = net::SimulationMode::kAuthoritative);
  void Shoot();
  void StartReload();
  // Full magazine and reserve, reload cancelled (respawn).
  void Refill();

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
  bool reloading() const { return state_.reloading; }
  bool tucked() const { return state_.tucked; }
  // How far the reload is, 0 to 1, extrapolated from the last tick.
  double ReloadProgress() const;
  // The model's animations (idle, fire, reload, draw; null: none).
  AnimationPlayer* animation() const { return animation_; }

  // The prefix of the arms' animations holding this weapon (a ViewModel
  // plays "<prefix>_idle" with this weapon's "idle").
  void set_view_animations(const String& prefix) { view_animations_ = prefix; }
  String view_animations() const { return view_animations_; }

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
  // The weapon tucks in when a wall is closer than the barrel reaches
  // (meters ahead of the aim, along the barrel's line kBarrelOffset from
  // it), and comes back out once it is kTuckRelease away (a little
  // farther, so standing at the edge doesn't flicker).
  static constexpr float kTuckDistance = 0.95f;
  static constexpr float kTuckRelease = 1.0f;
  static constexpr Vector3 kBarrelOffset = Vector3(0.17f, -0.2f, 0.0f);
  // What tucks it: the level, not the players (who move, and are drawn in
  // the past on clients, so predictions would miss).
  static constexpr uint32_t kTuckMask = kShotMask & ~(1u << 3);
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

  // Ray from aim's origin along its -Z, kRange long, not hitting the
  // owner nor the excluded bodies. Returns intersect_ray's result (empty:
  // no hit).
  Dictionary CastRay(const Transform3D& aim,
                     const TypedArray<RID>& exclude = TypedArray<RID>());
  // Whether a wall is in the barrel's way (with hysteresis).
  bool CheckTucked();
  // An authoritative shot's hit: the nearest of the level (the physics
  // ray) and, with a ShotResolver, its bodies. Sets the collider (null: a
  // miss), position and normal.
  Object* ResolveHit(const Transform3D& aim,
                     Vector3* position,
                     Vector3* normal);
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

  // The parent: shots come from its aim, and the view from its camera.
  FpsCharacter* owner_ = nullptr;
  // The model's animations.
  AnimationPlayer* animation_ = nullptr;
  String view_animations_;

  // Muzzle effect nodes (Flash, its Light, Sparks and Smoke under the
  // Muzzle, which follows the model's gun bone).
  Node3D* flash_ = nullptr;
  OmniLight3D* flash_light_ = nullptr;
  CPUParticles3D* sparks_ = nullptr;
  GPUParticles3D* smoke_ = nullptr;
  float flash_timer_ = 0.0f;
  float smoke_timer_ = 0.0f;
  Vector3 flash_scale_ = Vector3(1.0f, 1.0f, 1.0f);
  // The Muzzle's FireSound.
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
