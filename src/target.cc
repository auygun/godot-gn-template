#include "src/target.h"

#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "src/impact.h"

namespace godot {

Target::Target() = default;

Target::~Target() = default;

void Target::_ready() {
  spawn_transform_ = get_global_transform();
}

void Target::_physics_process(double delta) {
  float dt = static_cast<float>(delta);

  if (dead_) {
    respawn_timer_ -= dt;
    if (respawn_timer_ <= 0.0f) {
      Respawn();
    }
  }

  Vector3 vel = get_velocity();
  if (is_on_floor() && !dead_) {
    vel.x = static_cast<float>(
        UtilityFunctions::move_toward(vel.x, 0.0f, kFriction * dt));
    vel.z = static_cast<float>(
        UtilityFunctions::move_toward(vel.z, 0.0f, kFriction * dt));
  }
  vel += Vector3(0.0f, -kGravity, 0.0f) * dt;
  set_velocity(vel);
  move_and_slide();
}

void Target::TakeDamage(float amount, const Vector3& from) {
  if (dead_) {
    return;
  }
  health_ -= amount;

  Vector3 knockback = get_global_position() - from;
  knockback.y = 0.0f;
  knockback = knockback.normalized() * 3.0f;
  Vector3 vel = get_velocity();
  vel += knockback;
  set_velocity(vel);

  if (health_ <= 0.0f) {
    health_ = 0.0f;
    dead_ = true;
    respawn_timer_ = kRespawnTime;

    // Pop up, then fall through the floor (collision disabled).
    vel = get_velocity();
    vel += Vector3(0.0f, 6.0f, 0.0f);
    set_velocity(vel);

    CollisionShape3D* shape =
        Object::cast_to<CollisionShape3D>(get_node_or_null("CollisionShape3D"));
    if (shape) {
      shape->set_disabled(true);
    }
    emit_signal("died");
  }
}

void Target::Respawn() {
  dead_ = false;
  health_ = max_health_;
  set_velocity(Vector3());
  set_global_transform(spawn_transform_);
  // A teleport: don't draw the target sliding up from below the floor.
  reset_physics_interpolation();
  CollisionShape3D* shape =
      Object::cast_to<CollisionShape3D>(get_node_or_null("CollisionShape3D"));
  if (shape) {
    shape->set_disabled(false);
  }
  // Come back without the bullet holes.
  for (int i = 0; i < get_child_count(); i++) {
    Impact* impact = Object::cast_to<Impact>(get_child(i));
    if (impact) {
      impact->queue_free();
    }
  }
}

// static
void Target::_bind_methods() {
  ClassDB::bind_method(D_METHOD("take_damage", "amount", "from"),
                       &Target::TakeDamage);
  ClassDB::bind_method(D_METHOD("respawn"), &Target::Respawn);
  ClassDB::bind_method(D_METHOD("get_health"), &Target::health);

  ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "health"), "", "get_health");

  ADD_SIGNAL(MethodInfo("died"));
}

}  // namespace godot
