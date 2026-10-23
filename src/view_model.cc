#include "src/view_model.h"

#include <algorithm>
#include <cmath>

#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "src/fps_character.h"
#include "src/weapon.h"

namespace godot {

namespace {

// The arms' nodes and the bone the weapon follows (models/arms.glb).
constexpr char kSkeletonPath[] = "Arms/Arms/Skeleton3D";
constexpr char kAnimationPath[] = "Arms/AnimationPlayer";
constexpr char kWeaponBone[] = "weapon";

}  // namespace

ViewModel::ViewModel() = default;

ViewModel::~ViewModel() = default;

void ViewModel::_ready() {
  owner_ = Object::cast_to<FpsCharacter>(get_parent());
  skeleton_ = Object::cast_to<Skeleton3D>(get_node_or_null(kSkeletonPath));
  animation_ =
      Object::cast_to<AnimationPlayer>(get_node_or_null(kAnimationPath));
  if (!owner_ || !skeleton_ || !animation_) {
    set_process(false);
    set_physics_process(false);
    ERR_FAIL_MSG(
        "A ViewModel must be a child of an FpsCharacter, with the arms.");
  }
  weapon_bone_ = skeleton_->find_bone(kWeaponBone);
  ERR_FAIL_COND_MSG(weapon_bone_ < 0, "The arms have no weapon bone.");
  // Posed by UpdateAnimation, in step with the weapon's.
  animation_->set_callback_mode_process(
      AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL);
  // Placed every frame from the owner's view, which the camera shows
  // without interpolation too.
  set_as_top_level(true);
  set_physics_interpolation_mode(PHYSICS_INTERPOLATION_MODE_OFF);
  SetUpMeshes(this);
}

void ViewModel::Hold(Weapon* weapon) {
  if (weapon_ == weapon) {
    return;
  }
  if (weapon_) {
    weapon_->disconnect("fire_effects_played",
                        callable_mp(this, &ViewModel::OnWeaponFired));
    weapon_->set_as_top_level(false);
    weapon_->set_physics_interpolation_mode(PHYSICS_INTERPOLATION_MODE_INHERIT);
  }
  weapon_ = weapon;
  weapon_animation_ = nullptr;
  prefix_ = String();
  if (!weapon_) {
    return;
  }
  weapon_->connect("fire_effects_played",
                   callable_mp(this, &ViewModel::OnWeaponFired));
  // Placed on the weapon bone every frame (PlaceWeapon).
  weapon_->set_as_top_level(true);
  weapon_->set_physics_interpolation_mode(PHYSICS_INTERPOLATION_MODE_OFF);
  SetUpMeshes(weapon_);
  prefix_ = weapon_->view_animations() + "_";
  weapon_animation_ = weapon_->animation();
  if (weapon_animation_) {
    weapon_animation_->set_callback_mode_process(
        AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL);
  }
  Play("draw");
}

void ViewModel::_process(double delta) {
  float dt = static_cast<float>(delta);
  UpdatePose(dt);
  UpdateAnimation(dt);
  PlaceWeapon();
}

void ViewModel::_physics_process(double delta) {
  UpdateAimTarget();
}

void ViewModel::_notification(int what) {
  // Shown again: the owner respawned.
  if (what == NOTIFICATION_VISIBILITY_CHANGED && is_visible() && weapon_) {
    Play("draw");
  }
}

void ViewModel::UpdateAimTarget() {
  Ref<World3D> world_3d = get_world_3d();
  PhysicsDirectSpaceState3D* space_state =
      world_3d.is_valid() ? world_3d->get_direct_space_state() : nullptr;
  if (!space_state) {
    return;
  }
  // Cosmetic: the barrel points at what the crosshair (the camera) is on.
  Transform3D camera_xf = owner_->GetCameraTransform();
  Ref<PhysicsRayQueryParameters3D> query = PhysicsRayQueryParameters3D::create(
      camera_xf.origin, camera_xf.xform(Vector3(0.0f, 0.0f, -kMaxAimDistance)),
      Weapon::kShotMask);
  TypedArray<RID> exclude;
  exclude.push_back(owner_->get_rid());
  query->set_exclude(exclude);
  Dictionary result = space_state->intersect_ray(query);
  float hit_distance = kMaxAimDistance;
  if (result.has("position")) {
    Vector3 position = result.get("position", Variant());
    hit_distance = camera_xf.origin.distance_to(position);
  }
  aim_target_distance_ =
      static_cast<float>(UtilityFunctions::maxf(hit_distance, kMinAimDistance));
}

void ViewModel::UpdatePose(float dt) {
  Transform3D camera_xf = owner_->GetCameraTransform();
  // Where the arms hold the weapon at rest, from the camera.
  Vector3 grip = skeleton_->get_transform().xform(
      skeleton_->get_bone_global_rest(weapon_bone_).origin);

  // Bob and sway, from the owner's motion.
  float phase = owner_->step_phase();
  float amount = owner_->bob_amount();
  Vector3 bob = Vector3(kBobSide * std::sin(phase),
                        kBobUp * std::sin(2.0f * phase), 0.0f) *
                    amount +
                Vector3(0.0f, kLandingDip * owner_->landing_offset(), 0.0f);
  float roll = kBobRoll * amount * std::sin(phase);
  Vector2 look_sway_target =
      (-kLookSway * owner_->look_velocity()).limit_length(kMaxLookSway);
  Vector3 move_sway_target =
      (-kMoveSway * owner_->local_velocity()).limit_length(kMaxMoveSway);
  float sway_blend = 1.0f - std::exp(-kSwayRate * dt);
  look_sway_ += (look_sway_target - look_sway_) * sway_blend;
  move_sway_ += (move_sway_target - move_sway_) * sway_blend;

  Vector3 origin = camera_xf.xform(grip + bob + move_sway_);

  // Smoothed, so the barrel doesn't snap when the crosshair crosses an edge.
  aim_distance_ += (aim_target_distance_ - aim_distance_) *
                   (1.0f - std::exp(-kAimDistanceRate * dt));

  Vector3 forward = -camera_xf.basis.get_column(2);
  Vector3 up = camera_xf.basis.get_column(1);
  Vector3 aim_point = camera_xf.origin + forward * aim_distance_;
  Transform3D aim_xf(Basis::looking_at(aim_point - origin, up), origin);

  float decay = std::exp(-kKickReturn * dt);
  kick_back_ *= decay;
  kick_pitch_ *= decay;
  kick_yaw_ *= decay;
  // The weapon's pose, and the arms round it.
  Transform3D weapon_xf =
      aim_xf.translated_local(Vector3(0.0f, 0.0f, kGripOffset))
          .rotated_local(Vector3(0.0f, 1.0f, 0.0f), kick_yaw_ + look_sway_.x)
          .rotated_local(Vector3(1.0f, 0.0f, 0.0f), kick_pitch_ + look_sway_.y)
          .rotated_local(Vector3(0.0f, 0.0f, 1.0f), roll)
          .translated_local(Vector3(0.0f, 0.0f, kick_back_ - kGripOffset));
  set_global_transform(weapon_xf.translated_local(-grip));
}

void ViewModel::UpdateAnimation(float dt) {
  if (weapon_ && weapon_->reloading()) {
    // Follows the weapon's state, so a reload a misprediction or respawn
    // cuts short or restarts shows it.
    if (playing_ != StringName("reload")) {
      Play("reload");
    }
    progress_ = weapon_->ReloadProgress();
  } else {
    // Every animation starts and ends in idle's first pose, so idle repeats
    // (and the others return to it) by starting it again.
    if (playing_ == StringName("reload") || progress_ >= 1.0) {
      Play("idle");
    }
    progress_ =
        duration_ > 0.0 ? std::min(progress_ + dt / duration_, 1.0) : 1.0;
  }
  // Each clip at the same share of its own length, so clips of different
  // lengths start and end together.
  for (AnimationPlayer* player : {animation_, weapon_animation_}) {
    if (player && player->is_playing()) {
      player->seek(progress_ * player->get_current_animation_length(), true);
    }
  }
}

void ViewModel::Play(const StringName& name) {
  playing_ = name;
  progress_ = 0.0;
  duration_ = 0.0;
  StringName arms_name = prefix_ + String(name);
  if (animation_ && animation_->has_animation(arms_name)) {
    animation_->play(arms_name);
    animation_->seek(0.0, true);
    duration_ = animation_->get_current_animation_length();
  }
  if (weapon_animation_ && weapon_animation_->has_animation(name)) {
    weapon_animation_->play(name);
    weapon_animation_->seek(0.0, true);
    if (duration_ == 0.0) {
      duration_ = weapon_animation_->get_current_animation_length();
    }
  }
}

void ViewModel::PlaceWeapon() {
  if (weapon_) {
    weapon_->set_global_transform(
        skeleton_->get_global_transform() *
        skeleton_->get_bone_global_pose(weapon_bone_));
  }
}

void ViewModel::OnWeaponFired() {
  Play("fire");
  kick_back_ += kKickBack;
  kick_pitch_ += kKickPitch;
  kick_yaw_ +=
      static_cast<float>(UtilityFunctions::randf_range(-kKickYaw, kKickYaw));
}

// static
void ViewModel::SetUpMeshes(Node* node) {
  MeshInstance3D* mesh = Object::cast_to<MeshInstance3D>(node);
  if (mesh && mesh->get_mesh().is_valid()) {
    // A gun in front of the camera would shadow the floor ahead.
    mesh->set_cast_shadows_setting(
        GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
    for (int32_t i = 0; i < mesh->get_mesh()->get_surface_count(); i++) {
      Ref<BaseMaterial3D> material = mesh->get_active_material(i);
      if (material.is_null()) {
        continue;
      }
      material = material->duplicate();
      material->set_flag(BaseMaterial3D::FLAG_USE_Z_CLIP_SCALE, true);
      material->set_z_clip_scale(kZClipScale);
      mesh->set_surface_override_material(i, material);
    }
  }
  for (int32_t i = 0; i < node->get_child_count(); i++) {
    SetUpMeshes(node->get_child(i));
  }
}

// static
void ViewModel::_bind_methods() {}

}  // namespace godot
