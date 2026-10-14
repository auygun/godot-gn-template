#ifndef SRC_WEAPON_H_
#define SRC_WEAPON_H_

#include <array>
#include <cstdint>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

namespace godot {

class AudioStreamPlayer3D;
class Camera3D;
class CPUParticles3D;
class OmniLight3D;

class Weapon : public Node3D {
  GDCLASS(Weapon, Node3D);

 public:
  Weapon();
  ~Weapon() override;

  void _ready() override;
  void _process(double delta) override;
  void _physics_process(double delta) override;

  void Shoot();
  void StartReload();
  // Ignore the current "shoot" press (e.g. the click that recaptured the
  // mouse) until it is released.
  void HoldFire();
  // Full magazine and reserve, reload cancelled (respawn).
  void Refill();

  int ammo() const { return ammo_; }
  int reserve_ammo() const { return reserve_ammo_; }

  // Instanced where a shot hits; see Impact.
  void set_impact_scene(const Ref<PackedScene>& scene) {
    impact_scene_ = scene;
  }
  Ref<PackedScene> impact_scene() const { return impact_scene_; }

 protected:
  static void _bind_methods();

 private:
  // Length of the hitscan ray, in meters.
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

  // The viewport's current camera; null if there is none or the weapon is
  // outside the tree.
  Camera3D* ActiveCamera() const;
  // Ray from the camera's center through the crosshair. Returns
  // intersect_ray's result (empty: no hit).
  Dictionary CastAimRay(Camera3D* camera);
  // Measure how far the crosshair's hit point is (a physics query, so once
  // per tick), for UpdatePose to aim the barrel at.
  void UpdateAimTarget();
  // Follow the camera (keeping the scene's offset from it) and point the
  // barrel at what the crosshair is on.
  void UpdatePose(float dt);
  void ShowMuzzleFlash();
  void PlayFireSound();
  // Leave an impact on the collider at the hit point, facing along the
  // surface normal.
  void SpawnImpact(Object* collider,
                   const Vector3& position,
                   const Vector3& normal);

  // The weapon's transform relative to the camera, from the scene.
  Transform3D camera_offset_;
  bool has_camera_offset_ = false;
  float aim_target_distance_ = kRange;
  float aim_distance_ = kRange;
  float kick_back_ = 0.0f;
  float kick_pitch_ = 0.0f;
  float kick_yaw_ = 0.0f;

  // Muzzle effect nodes (Muzzle/Flash, its Light, Muzzle/Sparks,
  // Muzzle/Smoke).
  Node3D* flash_ = nullptr;
  OmniLight3D* flash_light_ = nullptr;
  CPUParticles3D* sparks_ = nullptr;
  CPUParticles3D* smoke_ = nullptr;
  float flash_timer_ = 0.0f;
  float smoke_timer_ = 0.0f;
  Vector3 flash_scale_ = Vector3(1.0f, 1.0f, 1.0f);
  // Muzzle/FireSound.
  AudioStreamPlayer3D* fire_sound_ = nullptr;

  Ref<PackedScene> impact_scene_;
  // Instance IDs of the live impacts, a ring buffer of impact_limit_
  // entries; stale IDs (impacts that freed themselves) resolve to null.
  std::array<uint64_t, kMaxImpacts> impact_ids_ = {};
  int impact_limit_ = kMaxImpacts;
  int next_impact_ = 0;

  bool reloading_ = false;
  float reload_timer_ = 0.0f;
  float fire_cooldown_ = 0.0f;
  bool fire_held_ = false;
  int start_reserve_ammo_ = 0;

  float damage_ = 25.0f;
  float fire_rate_ = 8.0f;  // shots per second
  float reload_time_ = 1.5f;
  int ammo_ = 30;
  int max_ammo_ = 30;
  int reserve_ammo_ = 300;
};

}  // namespace godot

#endif  // SRC_WEAPON_H_
