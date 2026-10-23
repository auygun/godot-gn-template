#ifndef SRC_VIEW_MODEL_H_
#define SRC_VIEW_MODEL_H_

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string_name.hpp>

namespace godot {

class AnimationPlayer;
class FpsCharacter;
class Skeleton3D;
class Weapon;

// The first-person arms of the FpsCharacter it is a child of, holding its
// weapon. Placed every frame from the owner's view (with bob, sway, the
// shots' kick, and the weapon aimed at what the crosshair is on); the
// weapon follows the arms' "weapon" bone. The arms play the held weapon's
// animations (its view_animations prefix: "rifle_idle", ...) in step with
// the weapon's own ("idle", ...): a reload from the weapon's state, a
// shot's when it fires, the draw when shown, idle otherwise. Purely local:
// nothing here is simulated.
class ViewModel : public Node3D {
  GDCLASS(ViewModel, Node3D);

 public:
  ViewModel();
  ~ViewModel() override;

  void _ready() override;
  void _process(double delta) override;
  void _physics_process(double delta) override;
  void _notification(int what);

  // Hold weapon (null: none), drawing it.
  void Hold(Weapon* weapon);
  Weapon* weapon() const { return weapon_; }

 protected:
  static void _bind_methods();

 private:
  // The weapon aims at the crosshair's hit point, but never closer than
  // this (pressed against a wall it would turn sideways), in meters.
  static constexpr float kMinAimDistance = 2.0f;
  static constexpr float kMaxAimDistance = 100.0f;
  // How fast the aim distance follows the crosshair's hit point, 1/s.
  static constexpr float kAimDistanceRate = 15.0f;
  // Each shot kicks the arms back and the muzzle up (and randomly
  // sideways), on top of the camera's recoil; decays back exponentially.
  static constexpr float kKickBack = 0.04f;    // meters per shot
  static constexpr float kKickPitch = 0.07f;   // radians per shot
  static constexpr float kKickYaw = 0.015f;    // max radians per shot
  static constexpr float kKickReturn = 12.0f;  // decay rate, 1/s
  // The kick rotates around the grip, this far behind the weapon.
  static constexpr float kGripOffset = 0.1f;  // meters
  // Walking bobs the arms in a figure eight on top of the camera's bob,
  // with a sideways rock; sizes at walk speed.
  static constexpr float kBobSide = 0.012f;  // meters
  static constexpr float kBobUp = 0.008f;    // meters
  static constexpr float kBobRoll = 0.02f;   // radians
  // The arms lag behind turning (radians per radian/s of turn rate,
  // capped) and behind moving (meters per m/s, capped), springing back at
  // kSwayRate, 1/s.
  static constexpr float kLookSway = 0.012f;
  static constexpr float kMaxLookSway = 0.08f;  // radians
  static constexpr float kMoveSway = 0.004f;
  static constexpr float kMaxMoveSway = 0.03f;  // meters
  static constexpr float kSwayRate = 10.0f;
  // Landing dips the arms this much more than the camera.
  static constexpr float kLandingDip = 0.6f;
  // The meshes' depth is squeezed towards the camera by this
  // (BaseMaterial3D::z_clip_scale), so they draw over the walls the
  // barrel pokes into.
  static constexpr float kZClipScale = 0.5f;

  // Measure how far the crosshair's hit point is (a physics query, so once
  // per tick).
  void UpdateAimTarget();
  // Follow the owner's view, and point the weapon at what the crosshair
  // is on.
  void UpdatePose(float dt);
  // Pose both animations at the same progress: a reload's from the
  // weapon's state, the others' from the time played.
  void UpdateAnimation(float dt);
  // Play name (the weapon's own; the arms' with its prefix) from its start.
  void Play(const StringName& name);
  // Place the held weapon on the arms' weapon bone.
  void PlaceWeapon();
  void OnWeaponFired();
  // Draw the meshes under node over walls, without shadows.
  static void SetUpMeshes(Node* node);

  // The parent: the view and its motion come from it.
  FpsCharacter* owner_ = nullptr;
  Weapon* weapon_ = nullptr;
  // The arms' (models/arms.glb) skeleton and animations, and the weapon
  // bone's index.
  Skeleton3D* skeleton_ = nullptr;
  AnimationPlayer* animation_ = nullptr;
  int32_t weapon_bone_ = -1;
  // The weapon's animations and their prefix on the arms.
  AnimationPlayer* weapon_animation_ = nullptr;
  String prefix_;
  // The playing animation (the weapon's name for it), how far it is (0 to
  // 1), and how long it lasts: the arms' clip, or the weapon's without one.
  StringName playing_;
  double progress_ = 0.0;
  double duration_ = 0.0;

  float aim_target_distance_ = kMaxAimDistance;
  float aim_distance_ = kMaxAimDistance;
  float kick_back_ = 0.0f;
  float kick_pitch_ = 0.0f;
  float kick_yaw_ = 0.0f;
  // Smoothed sway: x yaw, y pitch (radians); position in camera space.
  Vector2 look_sway_;
  Vector3 move_sway_;
};

}  // namespace godot

#endif  // SRC_VIEW_MODEL_H_
