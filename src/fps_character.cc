#include "src/fps_character.h"

#include <cmath>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math_defs.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "src/weapon.h"

namespace godot {

FpsCharacter::FpsCharacter() = default;

FpsCharacter::~FpsCharacter() = default;

void FpsCharacter::_ready() {
  camera_ = Object::cast_to<Camera3D>(get_node_or_null("Camera3D"));
  if (camera_) {
    // Placed every frame from the latest look input (see UpdateCamera);
    // interpolating it would add a tick of look latency.
    camera_position_ = camera_->get_position();
    camera_->set_as_top_level(true);
    camera_->set_physics_interpolation_mode(PHYSICS_INTERPOLATION_MODE_OFF);
  }
  weapon_ = Object::cast_to<Weapon>(get_node_or_null("Weapon"));
  if (weapon_) {
    weapon_->connect("fired", callable_mp(this, &FpsCharacter::OnWeaponFired));
  }
  spawn_transform_ = get_global_transform();
  yaw_ = get_rotation().y;
  Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
}

void FpsCharacter::_physics_process(double delta) {
  float dt = static_cast<float>(delta);

  float decay = std::exp(-kRecoilReturn * dt);
  recoil_ *= decay;
  recoil_yaw_ *= decay;

  // Turn the body by the look input since the last tick; the camera has
  // already turned (see _process).
  set_rotation(Vector3(0.0f, yaw_, 0.0f));

  Vector3 vel = get_velocity();

  if (is_on_floor()) {
    if (vel.y < 0.0f) {
      vel.y = 0.0f;
    }
    if (Input::get_singleton()->is_action_just_pressed("jump")) {
      vel.y = kJumpVelocity;
    }
  }
  vel += Vector3(0.0f, -kGravity, 0.0f) * dt;

  Vector2 input = Input::get_singleton()->get_vector(
      "move_left", "move_right", "move_forward", "move_back");
  float speed = Input::get_singleton()->is_action_pressed("sprint")
                    ? kSprintSpeed
                    : kWalkSpeed;

  Vector3 direction =
      get_transform().basis.xform(Vector3(input.x, 0.0f, input.y));
  if (direction.length_squared() > 0.0f) {
    direction = direction.normalized() * speed;
    vel.x = direction.x;
    vel.z = direction.z;
  } else {
    // 12.0: deceleration rate, 1/s (how quickly the player stops).
    vel.x = static_cast<float>(
        UtilityFunctions::move_toward(vel.x, 0.0f, speed * dt * 12.0f));
    vel.z = static_cast<float>(
        UtilityFunctions::move_toward(vel.z, 0.0f, speed * dt * 12.0f));
  }

  set_velocity(vel);
  move_and_slide();

  // Walked off the edge of the map.
  if (get_global_transform().origin.y < kFallLimit) {
    Respawn();
  }

  // Tick position, not interpolated: the weapon (a child) runs its
  // _physics_process next and casts its aim ray from the camera.
  // _process moves the camera to the drawn position before rendering.
  UpdateCamera(get_global_transform().origin, 0.0f);
}

void FpsCharacter::_process(double delta) {
  // Recoil only kicks and decays on ticks; decay it to now, so its return
  // is smooth at any refresh rate.
  float since_tick = static_cast<float>(
      Engine::get_singleton()->get_physics_interpolation_fraction() *
      get_physics_process_delta_time());
  // The body is drawn between its last two tick positions; the eye follows
  // it there, turned by the latest look input.
  UpdateCamera(get_global_transform_interpolated().origin, since_tick);
}

void FpsCharacter::_unhandled_input(const Ref<InputEvent>& event) {
  Input* input = Input::get_singleton();
  bool captured = input->get_mouse_mode() == Input::MOUSE_MODE_CAPTURED;

  if (event->is_class("InputEventMouseMotion")) {
    // A visible cursor (after ui_cancel) must not turn the view.
    if (captured) {
      // get_screen_relative() gives raw screen pixels; get_relative() is
      // scaled by the window stretch, which would make the sensitivity
      // depend on the window size.
      const Ref<InputEventMouseMotion> motion = event;
      ApplyLookDelta(motion->get_screen_relative(), kMouseSensitivity);
    }
  } else if (event->is_action_pressed("ui_cancel")) {
    input->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
  } else if (event->is_class("InputEventMouseButton")) {
    const Ref<InputEventMouseButton> button = event;
    if (button->is_pressed() && !captured) {
      input->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
      // The click only recaptures the mouse; don't let it fire.
      if (weapon_) {
        weapon_->HoldFire();
      }
    }
  }
}

void FpsCharacter::Respawn() {
  set_global_transform(spawn_transform_);
  // A teleport: don't draw the body sliding back across the map.
  reset_physics_interpolation();
  set_velocity(Vector3());
  yaw_ = get_rotation().y;
  pitch_ = 0.0f;
  recoil_ = 0.0f;
  recoil_yaw_ = 0.0f;
  UpdateCamera(get_global_transform().origin, 0.0f);
  if (weapon_) {
    weapon_->Refill();
  }
}

void FpsCharacter::ApplyLookDelta(const Vector2& delta, float sensitivity) {
  // Wrapped, so endless turning doesn't lose float precision.
  yaw_ = std::remainder(yaw_ - delta.x * sensitivity,
                        static_cast<float>(Math::TAU));
  pitch_ -= delta.y * sensitivity;
  pitch_ = static_cast<float>(
      UtilityFunctions::clampf(pitch_, -kPitchLimit, kPitchLimit));
}

void FpsCharacter::UpdateCamera(const Vector3& origin, float since_tick) {
  if (!camera_) {
    return;
  }
  float decay = std::exp(-kRecoilReturn * since_tick);
  float pitch = static_cast<float>(UtilityFunctions::clampf(
      pitch_ + recoil_ * decay, -kPitchLimit, kPitchLimit));
  Basis yaw(Vector3(0.0f, 1.0f, 0.0f), yaw_);
  camera_->set_global_transform(Transform3D(
      yaw * Basis::from_euler(Vector3(pitch, recoil_yaw_ * decay, 0.0f)),
      origin + yaw.xform(camera_position_)));
}

// static
void FpsCharacter::_bind_methods() {
  ClassDB::bind_method(D_METHOD("respawn"), &FpsCharacter::Respawn);
}

void FpsCharacter::OnWeaponFired() {
  recoil_ += kRecoilKick;
  recoil_yaw_ += static_cast<float>(
      UtilityFunctions::randf_range(-kRecoilYawKick, kRecoilYawKick));
}

}  // namespace godot
