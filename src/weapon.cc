#include "src/weapon.h"

#include <cmath>

#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/classes/area3d.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/cpu_particles3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/gpu_particles3d.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math_defs.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "src/fps_character.h"
#include "src/impact.h"
#include "src/target.h"

namespace godot {

namespace {

// The model's nodes: its animations, and the muzzle effects on its gun
// bone (weapon.tscn).
constexpr char kAnimationPath[] = "Model/AnimationPlayer";
constexpr char kMuzzlePath[] = "Model/Rifle/Skeleton3D/GunBone/Muzzle";

}  // namespace

Weapon::Weapon() = default;

Weapon::~Weapon() = default;

void Weapon::_ready() {
  owner_ = Object::cast_to<FpsCharacter>(get_parent());
  if (!owner_) {
    set_process(false);
    ERR_FAIL_MSG("A Weapon must be a child of an FpsCharacter.");
  }
  animation_ =
      Object::cast_to<AnimationPlayer>(get_node_or_null(kAnimationPath));
  if (animation_) {
    // At rest until a ViewModel plays it.
    animation_->stop();
  }
  Node* muzzle = get_node_or_null(kMuzzlePath);
  if (muzzle) {
    flash_ = Object::cast_to<Node3D>(muzzle->get_node_or_null("Flash"));
    flash_light_ =
        Object::cast_to<OmniLight3D>(muzzle->get_node_or_null("Flash/Light"));
    sparks_ =
        Object::cast_to<CPUParticles3D>(muzzle->get_node_or_null("Sparks"));
    smoke_ = Object::cast_to<GPUParticles3D>(muzzle->get_node_or_null("Smoke"));
    fire_sound_ = Object::cast_to<AudioStreamPlayer3D>(
        muzzle->get_node_or_null("FireSound"));
  }
  if (RenderingServer::get_singleton()->get_current_rendering_method() ==
      "mobile") {
    impact_limit_ = kMaxImpactsMobile;
  }
}

void Weapon::_process(double delta) {
  float dt = static_cast<float>(delta);

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

void Weapon::Tick(float dt,
                  bool fire,
                  bool reload_pressed,
                  net::SimulationMode mode) {
  if (state_.reloading) {
    state_.reload_timer -= dt;
    if (state_.reload_timer <= 0.0f) {
      state_.reloading = false;
      int needed = kMaxAmmo - state_.ammo;
      int take = needed < state_.reserve_ammo ? needed : state_.reserve_ammo;
      state_.ammo += take;
      state_.reserve_ammo -= take;
      emit_signal("ammo_changed", state_.ammo, state_.reserve_ammo,
                  state_.reloading);
    }
  }

  if (state_.fire_cooldown > 0.0f) {
    state_.fire_cooldown -= dt;
  }

  if (reload_pressed) {
    StartReload();
  }

  state_.tucked = CheckTucked();
  if (!state_.reloading && !state_.tucked && state_.fire_cooldown <= 0.0f &&
      fire) {
    if (state_.ammo > 0) {
      Fire(mode);
    } else {
      StartReload();
    }
  }
}

void Weapon::Shoot() {
  Fire(net::SimulationMode::kAuthoritative);
}

void Weapon::Fire(net::SimulationMode mode) {
  // Shoot() is bound: a script could call it on a weapon without an owner.
  ERR_FAIL_NULL(owner_);
  // Before "fired": the owner's recoil kick is for the next shot.
  Transform3D aim = owner_->GetAimTransform();
  state_.ammo--;
  // Accumulate rather than set: the cooldown overshoots below zero by up
  // to a frame, and resetting that would lower the fire rate.
  state_.fire_cooldown += 1.0f / fire_rate_;
  emit_signal("fired");
  emit_signal("ammo_changed", state_.ammo, state_.reserve_ammo,
              state_.reloading);
  if (mode == net::SimulationMode::kReplay) {
    return;
  }
  if (mode == net::SimulationMode::kPredicted) {
    PlayShotEffects(aim);
    return;
  }
  PlayFireEffects();

  Vector3 position;
  Vector3 normal;
  Object* collider = ResolveHit(aim, &position, &normal);
  if (collider) {
    emit_signal("hit", collider);
    SpawnImpact(collider, position, normal);
    Target* target = Object::cast_to<Target>(collider);
    if (target && !target->dead()) {
      target->TakeDamage(damage_, aim.origin);
      if (target->dead()) {
        emit_signal("killed", target);
      }
    }
    FpsCharacter* victim = Object::cast_to<FpsCharacter>(collider);
    if (victim && victim->TakeDamage(static_cast<int>(damage_))) {
      emit_signal("killed", victim);
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
  ShowMuzzleFlash();
  PlayFireSound();
  emit_signal("fire_effects_played");
}

Object* Weapon::ResolveHit(const Transform3D& aim,
                           Vector3* position,
                           Vector3* normal) {
  TypedArray<RID> exclude;
  if (shot_resolver_) {
    exclude = shot_resolver_->ShootableBodies();
  }
  Dictionary result = CastRay(aim, exclude);
  Object* collider = nullptr;
  float distance = kRange;
  if (result.has("collider")) {
    collider = result.get("collider", Variant());
    *position = result.get("position", Variant());
    *normal = result.get("normal", Variant());
    distance = aim.origin.distance_to(*position);
  }
  if (shot_resolver_) {
    Vector3 body_position;
    Vector3 body_normal;
    Node3D* body =
        shot_resolver_->ResolveShot(owner_, owner_->view_tick(), aim.origin,
                                    -aim.basis.get_column(2).normalized(),
                                    distance, &body_position, &body_normal);
    if (body) {
      collider = body;
      *position = body_position;
      *normal = body_normal;
    }
  }
  return collider;
}

Dictionary Weapon::CastRay(const Transform3D& aim,
                           const TypedArray<RID>& exclude_bodies) {
  Vector3 from = aim.origin;
  Vector3 to = aim.xform(Vector3(0.0f, 0.0f, -kRange));

  Ref<PhysicsRayQueryParameters3D> query =
      PhysicsRayQueryParameters3D::create(from, to, kShotMask);
  query->set_collide_with_areas(true);

  // Don't hit the player the weapon is attached to.
  TypedArray<RID> exclude;
  for (int64_t i = 0; i < exclude_bodies.size(); i++) {
    exclude.push_back(exclude_bodies[i]);
  }
  exclude.push_back(owner_->get_rid());
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

bool Weapon::CheckTucked() {
  if (!owner_) {
    return false;
  }
  Ref<World3D> world_3d = get_world_3d();
  PhysicsDirectSpaceState3D* space_state =
      world_3d.is_valid() ? world_3d->get_direct_space_state() : nullptr;
  if (!space_state) {
    return false;
  }
  // From the aim (game state), so the server and the predicting client
  // agree.
  Transform3D aim = owner_->GetAimTransform();
  Vector3 from = aim.xform(kBarrelOffset);
  Vector3 to = aim.xform(kBarrelOffset + Vector3(0.0f, 0.0f, -kTuckRelease));
  Ref<PhysicsRayQueryParameters3D> query =
      PhysicsRayQueryParameters3D::create(from, to, kTuckMask);
  TypedArray<RID> exclude;
  exclude.push_back(owner_->get_rid());
  query->set_exclude(exclude);
  Dictionary result = space_state->intersect_ray(query);
  if (!result.has("position")) {
    return false;
  }
  Vector3 position = result.get("position", Variant());
  return from.distance_to(position) <
         (state_.tucked ? kTuckRelease : kTuckDistance);
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
  if (state_.reloading || state_.ammo >= kMaxAmmo || state_.reserve_ammo <= 0) {
    return;
  }
  state_.reloading = true;
  state_.reload_timer = reload_time_;
  emit_signal("ammo_changed", state_.ammo, state_.reserve_ammo,
              state_.reloading);
}

void Weapon::Refill() {
  state_.reloading = false;
  state_.reload_timer = 0.0f;
  state_.fire_cooldown = 0.0f;
  state_.tucked = false;
  state_.ammo = kMaxAmmo;
  state_.reserve_ammo = kStartReserveAmmo;
  emit_signal("ammo_changed", state_.ammo, state_.reserve_ammo,
              state_.reloading);
}

double Weapon::ReloadProgress() const {
  if (!state_.reloading) {
    return 0.0;
  }
  Engine* engine = Engine::get_singleton();
  double elapsed = reload_time_ - state_.reload_timer +
                   engine->get_physics_interpolation_fraction() /
                       engine->get_physics_ticks_per_second();
  return UtilityFunctions::clampf(elapsed / reload_time_, 0.0, 1.0);
}

WeaponState Weapon::CaptureState() const {
  return state_;
}

void Weapon::RestoreState(const WeaponState& state) {
  bool changed = state.ammo != state_.ammo ||
                 state.reserve_ammo != state_.reserve_ammo ||
                 state.reloading != state_.reloading;
  state_ = state;
  if (changed) {
    emit_signal("ammo_changed", state_.ammo, state_.reserve_ammo,
                state_.reloading);
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
  ClassDB::bind_method(D_METHOD("set_view_animations", "prefix"),
                       &Weapon::set_view_animations);
  ClassDB::bind_method(D_METHOD("get_view_animations"),
                       &Weapon::view_animations);

  ADD_PROPERTY(PropertyInfo(Variant::INT, "ammo"), "", "get_ammo");
  ADD_PROPERTY(PropertyInfo(Variant::INT, "reserve_ammo"), "",
               "get_reserve_ammo");
  ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "impact_scene",
                            PROPERTY_HINT_RESOURCE_TYPE, "PackedScene"),
               "set_impact_scene", "get_impact_scene");
  ADD_PROPERTY(PropertyInfo(Variant::STRING, "view_animations"),
               "set_view_animations", "get_view_animations");

  ADD_SIGNAL(MethodInfo("fired"));
  // When a shot's flash and sound play (on the machines that show it).
  ADD_SIGNAL(MethodInfo("fire_effects_played"));
  ADD_SIGNAL(MethodInfo("hit", PropertyInfo(Variant::OBJECT, "collider")));
  // A Target or an FpsCharacter.
  ADD_SIGNAL(MethodInfo("killed", PropertyInfo(Variant::OBJECT, "victim")));
  ADD_SIGNAL(MethodInfo("ammo_changed", PropertyInfo(Variant::INT, "ammo"),
                        PropertyInfo(Variant::INT, "reserve"),
                        PropertyInfo(Variant::BOOL, "reloading")));
}

}  // namespace godot
