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

  bool was_on_floor = is_on_floor();
  Vector3 vel = get_velocity();

  if (was_on_floor) {
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

  // get_vector already limits the length to 1.
  Vector3 target =
      get_transform().basis.xform(Vector3(input.x, 0.0f, input.y)) * speed;
  // Airborne without input keeps its momentum.
  if (was_on_floor || !target.is_zero_approx()) {
    float accel = was_on_floor ? kGroundAccel : kAirAccel;
    Vector3 horizontal =
        Vector3(vel.x, 0.0f, vel.z).move_toward(target, accel * dt);
    vel.x = horizontal.x;
    vel.z = horizontal.z;
  }

  // The velocity going into the move: move_and_slide zeroes it against the
  // floor it lands on.
  float fall_speed = -vel.y;
  set_velocity(vel);
  move_and_slide();

  bool landed = !was_on_floor && is_on_floor();
  UpdateLandingDip(dt, landed ? fall_speed : 0.0f);
  // The actual velocity: walking into a wall doesn't bob.
  local_velocity_ = get_global_basis().xform_inv(get_real_velocity());

  // Walked off the edge of the map.
  if (get_global_transform().origin.y < kFallLimit) {
    Respawn();
  }

  // Tick position, not interpolated: the weapon (a child) runs its
  // _physics_process next and casts its aim ray from the camera.
  // _process moves the camera to the drawn position before rendering.
  UpdateCamera(get_global_transform().origin, landing_offset_, 0.0f);
}

void FpsCharacter::_process(double delta) {
  float dt = static_cast<float>(delta);
  if (dt > 0.0f) {
    look_velocity_ = look_delta_ / dt;
    look_delta_ = Vector2();
    UpdateBob(dt);
  }
  // Recoil only kicks and decays on ticks; decay it to now, so its return
  // is smooth at any refresh rate.
  float since_tick = static_cast<float>(
      Engine::get_singleton()->get_physics_interpolation_fraction() *
      get_physics_process_delta_time());
  // The body is drawn between its last two tick positions; the eye follows
  // it there, turned by the latest look input.
  UpdateCamera(get_global_transform_interpolated().origin, landing_offset(),
               since_tick);
}

float FpsCharacter::landing_offset() const {
  // Between the last two ticks, like the body.
  float fraction = static_cast<float>(
      Engine::get_singleton()->get_physics_interpolation_fraction());
  return prev_landing_offset_ +
         (landing_offset_ - prev_landing_offset_) * fraction;
}

void FpsCharacter::UpdateLandingDip(float dt, float landing_speed) {
  prev_landing_offset_ = landing_offset_;
  if (landing_speed > kMinLandingSpeed) {
    landing_velocity_ -=
        kLandingImpulse * static_cast<float>(UtilityFunctions::minf(
                              landing_speed, kMaxLandingSpeed));
  }
  // A critically damped spring back to rest, solved exactly.
  float w = kLandingSpring;
  float c = landing_velocity_ + w * landing_offset_;
  float e = std::exp(-w * dt);
  landing_offset_ = (landing_offset_ + c * dt) * e;
  landing_velocity_ = (landing_velocity_ - w * c * dt) * e;
}

void FpsCharacter::UpdateBob(float dt) {
  bool grounded = is_on_floor();
  float speed = Vector2(local_velocity_.x, local_velocity_.z).length();
  float target_bob = grounded ? static_cast<float>(UtilityFunctions::minf(
                                    speed / kWalkSpeed, kMaxBobAmount))
                              : 0.0f;
  bob_amount_ +=
      (target_bob - bob_amount_) * (1.0f - std::exp(-kBobBlendRate * dt));
  if (grounded) {
    step_phase_ =
        std::fmod(step_phase_ + static_cast<float>(Math::TAU) * kStepRate *
                                    std::sqrt(speed / kWalkSpeed) * dt,
                  static_cast<float>(Math::TAU));
  }
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
  bob_amount_ = 0.0f;
  landing_offset_ = 0.0f;
  landing_velocity_ = 0.0f;
  prev_landing_offset_ = 0.0f;
  look_delta_ = Vector2();
  look_velocity_ = Vector2();
  local_velocity_ = Vector3();
  UpdateCamera(get_global_transform().origin, landing_offset_, 0.0f);
  if (weapon_) {
    weapon_->Refill();
  }
}

void FpsCharacter::ApplyLookDelta(const Vector2& delta, float sensitivity) {
  float yaw = -delta.x * sensitivity;
  // Wrapped, so endless turning doesn't lose float precision.
  yaw_ = std::remainder(yaw_ + yaw, static_cast<float>(Math::TAU));
  float pitch = static_cast<float>(UtilityFunctions::clampf(
      pitch_ - delta.y * sensitivity, -kPitchLimit, kPitchLimit));
  // Only the rotation that happened: none against the pitch limit.
  look_delta_ += Vector2(yaw, pitch - pitch_);
  pitch_ = pitch;
}

void FpsCharacter::UpdateCamera(const Vector3& origin,
                                float landing_dip,
                                float since_tick) {
  if (!camera_) {
    return;
  }
  float decay = std::exp(-kRecoilReturn * since_tick);
  float pitch = static_cast<float>(UtilityFunctions::clampf(
      pitch_ + recoil_ * decay, -kPitchLimit, kPitchLimit));
  // The head is lowest when a foot lands (phase 0 and pi).
  float bob = kBobUp * bob_amount_ * -std::cos(2.0f * step_phase_);
  Basis yaw(Vector3(0.0f, 1.0f, 0.0f), yaw_);
  camera_->set_global_transform(Transform3D(
      yaw * Basis::from_euler(Vector3(pitch, recoil_yaw_ * decay, 0.0f)),
      origin + yaw.xform(camera_position_ +
                         Vector3(0.0f, bob + landing_dip, 0.0f))));
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
