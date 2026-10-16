#include "src/weapon.h"

#include <cmath>

#include <godot_cpp/classes/area3d.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/cpu_particles3d.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/physics_body3d.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math_defs.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "src/fps_character.h"
#include "src/impact.h"
#include "src/target.h"

namespace godot {

Weapon::Weapon() = default;

Weapon::~Weapon() = default;

void Weapon::_ready() {
  start_reserve_ammo_ = reserve_ammo_;
  flash_ = Object::cast_to<Node3D>(get_node_or_null("Muzzle/Flash"));
  flash_light_ =
      Object::cast_to<OmniLight3D>(get_node_or_null("Muzzle/Flash/Light"));
  sparks_ = Object::cast_to<CPUParticles3D>(get_node_or_null("Muzzle/Sparks"));
  smoke_ = Object::cast_to<CPUParticles3D>(get_node_or_null("Muzzle/Smoke"));
  fire_sound_ = Object::cast_to<AudioStreamPlayer3D>(
      get_node_or_null("Muzzle/FireSound"));
  if (RenderingServer::get_singleton()->get_current_rendering_method() ==
      "mobile") {
    impact_limit_ = kMaxImpactsMobile;
  }

  // An owner sets the camera from its _ready, which runs after this one.
  if (!Object::cast_to<FpsCharacter>(get_parent())) {
    SetCamera(get_viewport()->get_camera_3d());
  }
}

void Weapon::SetCamera(Camera3D* camera) {
  camera_ = camera;
  if (!camera_) {
    return;
  }
  camera_offset_ =
      camera_->get_global_transform().affine_inverse() * get_global_transform();
  // Placed every frame from the camera (see UpdatePose), which isn't
  // interpolated either.
  set_as_top_level(true);
  set_physics_interpolation_mode(PHYSICS_INTERPOLATION_MODE_OFF);
}

void Weapon::_process(double delta) {
  float dt = static_cast<float>(delta);

  // The owner's _process (which runs first) has already placed the camera
  // this frame.
  UpdatePose(dt);

  // Stopping emission lets the puffs already in the air finish.
  if (smoke_ && smoke_->is_emitting()) {
    smoke_timer_ -= dt;
    if (smoke_timer_ <= 0.0f) {
      smoke_->set_emitting(false);
    }
  }

  if (!flash_ || !flash_->is_visible()) {
    return;
  }
  flash_timer_ -= dt;
  if (flash_timer_ <= 0.0f) {
    flash_->set_visible(false);
    return;
  }
  float t = flash_timer_ / kFlashTime;
  float scale = 0.5f + 0.5f * t;
  flash_->set_scale(flash_scale_ * scale);
  if (flash_light_) {
    flash_light_->set_param(Light3D::PARAM_ENERGY, kFlashLightEnergy * t);
  }
}

void Weapon::_physics_process(double delta) {
  // The owner's _physics_process (which runs first) has already turned the
  // camera this tick.
  UpdateAimTarget();
}

void Weapon::Tick(float dt, bool fire, bool reload_pressed) {
  if (reloading_) {
    reload_timer_ -= dt;
    if (reload_timer_ <= 0.0f) {
      reloading_ = false;
      int needed = max_ammo_ - ammo_;
      int take = needed < reserve_ammo_ ? needed : reserve_ammo_;
      ammo_ += take;
      reserve_ammo_ -= take;
      emit_signal("ammo_changed", ammo_, reserve_ammo_, reloading_);
    }
  }

  if (fire_cooldown_ > 0.0f) {
    fire_cooldown_ -= dt;
  }

  if (reload_pressed) {
    StartReload();
  }

  if (!reloading_ && fire_cooldown_ <= 0.0f && fire) {
    if (ammo_ > 0) {
      Shoot();
    } else {
      StartReload();
    }
  }
}

void Weapon::Shoot() {
  // Before "fired": the owner's recoil kick is for the next shot.
  Transform3D aim = ShotTransform();
  ammo_--;
  // Accumulate rather than set: the cooldown overshoots below zero by up
  // to a frame, and resetting that would lower the fire rate.
  fire_cooldown_ += 1.0f / fire_rate_;
  emit_signal("fired");
  emit_signal("ammo_changed", ammo_, reserve_ammo_, reloading_);
  PlayFireEffects();

  Dictionary result = CastRay(aim);
  if (result.has("collider")) {
    Object* collider = result.get("collider", Variant());
    emit_signal("hit", collider);
    SpawnImpact(collider, result.get("position", Variant()),
                result.get("normal", Variant()));
    Target* target = Object::cast_to<Target>(collider);
    if (target && !target->dead()) {
      target->TakeDamage(damage_, aim.origin);
      if (target->dead()) {
        emit_signal("killed", target);
      }
    }
  }
}

void Weapon::PlayShotEffects(const Transform3D& aim) {
  PlayFireEffects();
  Dictionary result = CastRay(aim);
  if (result.has("collider")) {
    SpawnImpact(result.get("collider", Variant()),
                result.get("position", Variant()),
                result.get("normal", Variant()));
  }
}

void Weapon::PlayFireEffects() {
  // The kick only shows (and decays) on a weapon that follows a camera.
  if (camera_) {
    kick_back_ += kKickBack;
    kick_pitch_ += kKickPitch;
    kick_yaw_ +=
        static_cast<float>(UtilityFunctions::randf_range(-kKickYaw, kKickYaw));
  }
  ShowMuzzleFlash();
  PlayFireSound();
}

Transform3D Weapon::ShotTransform() const {
  FpsCharacter* owner_character = Object::cast_to<FpsCharacter>(get_parent());
  return owner_character ? owner_character->AimTransform()
                         : get_global_transform();
}

Dictionary Weapon::CastRay(const Transform3D& aim) {
  Vector3 from = aim.origin;
  Vector3 to = aim.xform(Vector3(0.0f, 0.0f, -kRange));

  Ref<PhysicsRayQueryParameters3D> query =
      PhysicsRayQueryParameters3D::create(from, to, kShotMask);
  query->set_collide_with_areas(true);

  // Don't hit the player the weapon is attached to.
  TypedArray<RID> exclude;
  PhysicsBody3D* owner_body = Object::cast_to<PhysicsBody3D>(get_parent());
  if (owner_body) {
    exclude.push_back(owner_body->get_rid());
  }
  query->set_exclude(exclude);

  Ref<World3D> world_3d = get_world_3d();
  if (!world_3d.is_valid()) {
    return Dictionary();
  }
  PhysicsDirectSpaceState3D* space_state = world_3d->get_direct_space_state();
  if (!space_state) {
    return Dictionary();
  }
  return space_state->intersect_ray(query);
}

void Weapon::UpdateAimTarget() {
  if (!camera_) {
    return;
  }
  // Cosmetic: the barrel points at what the crosshair (the camera) is on.
  Transform3D camera_xf = camera_->get_global_transform();
  float hit_distance = kRange;
  Dictionary result = CastRay(camera_xf);
  if (result.has("position")) {
    Vector3 position = result.get("position", Variant());
    hit_distance = camera_xf.origin.distance_to(position);
  }
  aim_target_distance_ =
      static_cast<float>(UtilityFunctions::maxf(hit_distance, kMinAimDistance));
}

void Weapon::UpdatePose(float dt) {
  if (!camera_) {
    return;
  }
  Transform3D camera_xf = camera_->get_global_transform();

  // Bob and sway, from the owner's motion; none without one.
  Vector3 bob;
  float roll = 0.0f;
  Vector2 look_sway_target;
  Vector3 move_sway_target;
  FpsCharacter* owner_character = Object::cast_to<FpsCharacter>(get_parent());
  if (owner_character) {
    float phase = owner_character->step_phase();
    float amount = owner_character->bob_amount();
    bob = Vector3(kBobSide * std::sin(phase), kBobUp * std::sin(2.0f * phase),
                  0.0f) *
              amount +
          Vector3(0.0f, kLandingDip * owner_character->landing_offset(), 0.0f);
    roll = kBobRoll * amount * std::sin(phase);
    look_sway_target = (-kLookSway * owner_character->look_velocity())
                           .limit_length(kMaxLookSway);
    move_sway_target = (-kMoveSway * owner_character->local_velocity())
                           .limit_length(kMaxMoveSway);
  }
  float sway_blend = 1.0f - std::exp(-kSwayRate * dt);
  look_sway_ += (look_sway_target - look_sway_) * sway_blend;
  move_sway_ += (move_sway_target - move_sway_) * sway_blend;

  Vector3 origin = camera_xf.xform(camera_offset_.origin + bob + move_sway_);

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
  set_global_transform(
      aim_xf.translated_local(Vector3(0.0f, 0.0f, kGripOffset))
          .rotated_local(Vector3(0.0f, 1.0f, 0.0f), kick_yaw_ + look_sway_.x)
          .rotated_local(Vector3(1.0f, 0.0f, 0.0f), kick_pitch_ + look_sway_.y)
          .rotated_local(Vector3(0.0f, 0.0f, 1.0f), roll)
          .translated_local(Vector3(0.0f, 0.0f, kick_back_ - kGripOffset)));
}

void Weapon::ShowMuzzleFlash() {
  if (flash_) {
    // A random roll and size so consecutive shots don't look identical.
    float roll =
        static_cast<float>(UtilityFunctions::randf_range(0.0, Math::TAU));
    float size = static_cast<float>(UtilityFunctions::randf_range(0.8, 1.2));
    float length = static_cast<float>(UtilityFunctions::randf_range(0.7, 1.3));
    flash_scale_ = Vector3(size, size, size * length);
    flash_->set_rotation(Vector3(0.0f, 0.0f, roll));
    flash_->set_scale(flash_scale_);
    flash_->set_visible(true);
    flash_timer_ = kFlashTime;
    if (flash_light_) {
      flash_light_->set_param(Light3D::PARAM_ENERGY, kFlashLightEnergy);
    }
  }
  if (sparks_) {
    sparks_->restart();
  }
  if (smoke_) {
    // Not restart(): that would clear the smoke still in the air.
    smoke_->set_emitting(true);
    smoke_timer_ = kSmokeTime;
  }
}

void Weapon::PlayFireSound() {
  // The player's max_polyphony lets each shot ring out instead of cutting
  // off the previous one; the stream varies pitch and volume per shot.
  if (fire_sound_ && fire_sound_->is_inside_tree()) {
    fire_sound_->play();
  }
}

void Weapon::SpawnImpact(Object* collider,
                         const Vector3& position,
                         const Vector3& normal) {
  // Areas are invisible, players don't keep bullet holes, and the normal is
  // zero when the ray starts inside a shape.
  Node3D* surface = Object::cast_to<Node3D>(collider);
  if (!impact_scene_.is_valid() || !surface ||
      Object::cast_to<Area3D>(collider) ||
      Object::cast_to<FpsCharacter>(collider) || normal.is_zero_approx()) {
    return;
  }
  Node3D* impact = Object::cast_to<Node3D>(impact_scene_->instantiate());
  if (!impact) {
    return;
  }

  Impact* oldest = Object::cast_to<Impact>(
      ObjectDB::get_instance(impact_ids_[next_impact_]));
  if (oldest) {
    oldest->queue_free();
  }
  impact_ids_[next_impact_] = impact->get_instance_id();
  next_impact_ = (next_impact_ + 1) % impact_limit_;

  // Build a basis with local +Y along the surface normal. The cross
  // product degenerates if the reference axis is parallel to the normal,
  // so use Z when the normal is near-vertical (the floor), else UP.
  Vector3 y = normal.normalized();
  Vector3 ref = std::abs(y.y) > 0.99f ? Vector3(0.0f, 0.0f, 1.0f)
                                      : Vector3(0.0f, 1.0f, 0.0f);
  Vector3 x = y.cross(ref).normalized();
  Vector3 z = x.cross(y);

  // Parent it to the surface, so a hole on a knocked-back target moves
  // with it. Set the transform before add_child: the particles start when
  // the node enters the tree, so it must be placed first.
  impact->set_transform(surface->get_global_transform().affine_inverse() *
                        Transform3D(Basis(x, y, z), position));
  surface->add_child(impact);
}

void Weapon::StartReload() {
  if (reloading_ || ammo_ >= max_ammo_ || reserve_ammo_ <= 0) {
    return;
  }
  reloading_ = true;
  reload_timer_ = reload_time_;
  emit_signal("ammo_changed", ammo_, reserve_ammo_, reloading_);
}

void Weapon::Refill() {
  reloading_ = false;
  reload_timer_ = 0.0f;
  fire_cooldown_ = 0.0f;
  kick_back_ = 0.0f;
  kick_pitch_ = 0.0f;
  kick_yaw_ = 0.0f;
  look_sway_ = Vector2();
  move_sway_ = Vector3();
  ammo_ = max_ammo_;
  reserve_ammo_ = start_reserve_ammo_;
  emit_signal("ammo_changed", ammo_, reserve_ammo_, reloading_);
}

WeaponState Weapon::CaptureState() const {
  WeaponState state;
  state.ammo = ammo_;
  state.reserve_ammo = reserve_ammo_;
  state.reloading = reloading_;
  state.reload_timer = reload_timer_;
  state.fire_cooldown = fire_cooldown_;
  return state;
}

void Weapon::ApplyState(const WeaponState& state) {
  bool changed = state.ammo != ammo_ || state.reserve_ammo != reserve_ammo_ ||
                 state.reloading != reloading_;
  ammo_ = state.ammo;
  reserve_ammo_ = state.reserve_ammo;
  reloading_ = state.reloading;
  reload_timer_ = state.reload_timer;
  fire_cooldown_ = state.fire_cooldown;
  if (changed) {
    emit_signal("ammo_changed", ammo_, reserve_ammo_, reloading_);
  }
}

// static
void Weapon::_bind_methods() {
  ClassDB::bind_method(D_METHOD("shoot"), &Weapon::Shoot);
  ClassDB::bind_method(D_METHOD("start_reload"), &Weapon::StartReload);
  ClassDB::bind_method(D_METHOD("refill"), &Weapon::Refill);
  ClassDB::bind_method(D_METHOD("get_ammo"), &Weapon::ammo);
  ClassDB::bind_method(D_METHOD("get_reserve_ammo"), &Weapon::reserve_ammo);
  ClassDB::bind_method(D_METHOD("set_impact_scene", "scene"),
                       &Weapon::set_impact_scene);
  ClassDB::bind_method(D_METHOD("get_impact_scene"), &Weapon::impact_scene);

  ADD_PROPERTY(PropertyInfo(Variant::INT, "ammo"), "", "get_ammo");
  ADD_PROPERTY(PropertyInfo(Variant::INT, "reserve_ammo"), "",
               "get_reserve_ammo");
  ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "impact_scene",
                            PROPERTY_HINT_RESOURCE_TYPE, "PackedScene"),
               "set_impact_scene", "get_impact_scene");

  ADD_SIGNAL(MethodInfo("fired"));
  ADD_SIGNAL(MethodInfo("hit", PropertyInfo(Variant::OBJECT, "collider")));
  ADD_SIGNAL(MethodInfo("killed", PropertyInfo(Variant::OBJECT, "target")));
  ADD_SIGNAL(MethodInfo("ammo_changed", PropertyInfo(Variant::INT, "ammo"),
                        PropertyInfo(Variant::INT, "reserve"),
                        PropertyInfo(Variant::BOOL, "reloading")));
}

}  // namespace godot
