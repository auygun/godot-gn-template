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

namespace {

// A well-mixed value in [-1, 1] for each n (a 32-bit integer hash), so
// "random" game state comes out the same on every machine.
float SignedNoise(uint32_t n) {
  n ^= n >> 16;
  n *= 0x7feb352du;
  n ^= n >> 15;
  n *= 0x846ca68bu;
  n ^= n >> 16;
  return static_cast<float>(n) / 2147483647.5f - 1.0f;
}

}  // namespace

FpsCharacter::FpsCharacter() = default;

FpsCharacter::~FpsCharacter() = default;

void FpsCharacter::_ready() {
  camera_ = Object::cast_to<Camera3D>(get_node_or_null("Camera3D"));
  if (camera_) {
    // Placed every frame from the latest look input (see UpdateCamera);
    // interpolating it would add a tick of look latency.
    eye_offset_ = camera_->get_position();
  }
  weapon_ = Object::cast_to<Weapon>(get_node_or_null("Weapon"));
  if (weapon_) {
    weapon_->connect("fired", callable_mp(this, &FpsCharacter::OnWeaponFired));
    // The weapon's _ready ran first, before the camera was set up.
    weapon_->SetCamera(camera_);
  }
  if (camera_) {
    camera_->set_as_top_level(true);
    camera_->set_physics_interpolation_mode(PHYSICS_INTERPOLATION_MODE_OFF);
  }
  floor_snap_length_ = get_floor_snap_length();
  spawn_transform_ = get_global_transform();
  yaw_ = get_rotation().y;
  look_yaw_ = yaw_;
  if (!local_) {
    // Drawn from the simulation alone: no look, bob or local input.
    set_process(false);
    set_process_unhandled_input(false);
    return;
  }
  Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
}

void FpsCharacter::set_local(bool local) {
  ERR_FAIL_COND_MSG(is_inside_tree(), "set_local() before adding to the tree");
  local_ = local;
  if (local_) {
    return;
  }
  // Freed before entering the tree: the camera is current in the scene,
  // and would take over the view from the local player's.
  for (const char* name : {"Camera3D", "TouchControls"}) {
    Node* child = get_node_or_null(name);
    if (child) {
      remove_child(child);
      memdelete(child);
    }
  }
}

void FpsCharacter::_physics_process(double delta) {
  // The NetSession has already simulated (or, on a client, applied the
  // server's state for) this tick.
  prev_correction_offset_ = correction_offset_;
  correction_offset_ =
      correction_offset_ *
          std::exp(-kCorrectionRate * static_cast<float>(delta)) +
      new_correction_;
  new_correction_ = Vector3();
  // Tick position, not interpolated: the weapon (a child) runs its
  // _physics_process next and aims the barrel from the camera.
  // _process moves the camera to the drawn position before rendering.
  UpdateCamera(get_global_transform().origin, landing_offset_,
               correction_offset_, 0.0f);
}

PlayerInput FpsCharacter::GatherInput() {
  Input* input = Input::get_singleton();
  // A tap that starts and ends between two ticks still counts: held for
  // this command.
  auto held = [input](const StringName& action) {
    return input->is_action_pressed(action) ||
           input->is_action_just_pressed(action);
  };
  if (fire_held_ && !held("shoot")) {
    fire_held_ = false;
  }

  PlayerInput command;
  command.move =
      input->get_vector("move_left", "move_right", "move_forward", "move_back");
  command.yaw = look_yaw_;
  command.pitch = look_pitch_;
  command.respawn_count = respawn_count_;
  command.jump = held("jump");
  command.sprint = input->is_action_pressed("sprint");
  command.fire = !fire_held_ && held("shoot");
  command.reload = held("reload");
  return command;
}

PlayerInput FpsCharacter::IdleInput() const {
  PlayerInput command;
  command.yaw = yaw_;
  command.pitch = pitch_;
  command.respawn_count = respawn_count_;
  return command;
}

void FpsCharacter::Simulate(const PlayerInput& command,
                            const net::SimContext& context) {
  float dt = context.dt;
  net::SimulationMode mode = context.mode;
  // The command may come over the network: keep it in range.
  PlayerInput input = command;
  input.move =
      input.move.is_finite() ? input.move.limit_length(1.0f) : Vector2();
  bool current_look = input.respawn_count == respawn_count_;
  if (current_look && std::isfinite(input.yaw)) {
    yaw_ = std::remainder(input.yaw, static_cast<float>(Math::TAU));
  }
  if (current_look && std::isfinite(input.pitch)) {
    pitch_ = static_cast<float>(
        UtilityFunctions::clampf(input.pitch, -kPitchLimit, kPitchLimit));
  }
  view_tick_ = context.view_tick;
  if (dead_) {
    // Lying where it died until the server respawns it; what the player
    // presses meanwhile mustn't count as a press after.
    prev_input_ = PlayerInput();
    return;
  }
  bool jump_pressed = input.jump && !prev_input_.jump;
  bool reload_pressed = input.reload && !prev_input_.reload;
  prev_input_ = input;

  float decay = std::exp(-kRecoilReturn * dt);
  recoil_ *= decay;
  recoil_yaw_ *= decay;

  set_rotation(Vector3(0.0f, yaw_, 0.0f));

  bool was_on_floor = on_floor_;
  Vector3 vel = get_velocity();

  if (was_on_floor) {
    if (vel.y < 0.0f) {
      vel.y = 0.0f;
    }
    if (jump_pressed) {
      vel.y = kJumpVelocity;
    }
  }
  vel += Vector3(0.0f, -kGravity, 0.0f) * dt;

  float speed = input.sprint ? kSprintSpeed : kWalkSpeed;
  Vector3 target =
      get_transform().basis.xform(Vector3(input.move.x, 0.0f, input.move.y)) *
      speed;
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
  // move_and_slide snaps to the floor if its last call ended on one; going
  // by on_floor_ instead keeps the result a function of the game state
  // (the last call may be a later tick, when replaying).
  set_floor_snap_length(was_on_floor ? floor_snap_length_ : 0.0f);
  move_and_slide();

  on_floor_ = is_on_floor();
  bool landed = !was_on_floor && on_floor_;
  UpdateLandingDip(dt, landed ? fall_speed : 0.0f);
  // The actual velocity: walking into a wall doesn't bob.
  local_velocity_ = get_global_basis().xform_inv(get_real_velocity());

  // Walked off the edge of the map.
  if (mode == net::SimulationMode::kAuthoritative &&
      get_global_transform().origin.y < kFallLimit) {
    Respawn();
  }

  // After moving, so shots leave from where the player now is.
  if (weapon_) {
    weapon_->Tick(dt, input.fire, reload_pressed, mode);
  }
}

Transform3D FpsCharacter::AimTransform() const {
  float pitch = static_cast<float>(
      UtilityFunctions::clampf(pitch_ + recoil_, -kPitchLimit, kPitchLimit));
  Basis yaw(Vector3(0.0f, 1.0f, 0.0f), yaw_);
  return Transform3D(
      yaw * Basis::from_euler(Vector3(pitch, recoil_yaw_, 0.0f)),
      get_global_position() +
          yaw.xform(eye_offset_ + Vector3(0.0f, landing_offset_, 0.0f)));
}

PlayerState FpsCharacter::CaptureState() const {
  PlayerState state;
  state.position = get_global_position();
  state.velocity = get_velocity();
  state.yaw = yaw_;
  state.pitch = pitch_;
  state.recoil = recoil_;
  state.recoil_yaw = recoil_yaw_;
  state.landing_offset = landing_offset_;
  state.landing_velocity = landing_velocity_;
  state.shot_count = shot_count_;
  state.respawn_count = respawn_count_;
  state.on_floor = on_floor_;
  state.health = static_cast<uint8_t>(health_);
  state.dead = dead_;
  state.prev_input = prev_input_;
  if (weapon_) {
    state.weapon = weapon_->CaptureState();
  }
  state.kills = static_cast<uint16_t>(kills_);
  state.deaths = static_cast<uint16_t>(deaths_);
  return state;
}

void FpsCharacter::RestoreState(const PlayerState& state) {
  yaw_ = state.yaw;
  pitch_ = state.pitch;
  set_global_transform(
      Transform3D(Basis(Vector3(0.0f, 1.0f, 0.0f), yaw_), state.position));
  set_velocity(state.velocity);
  recoil_ = state.recoil;
  recoil_yaw_ = state.recoil_yaw;
  prev_landing_offset_ = landing_offset_;
  landing_offset_ = state.landing_offset;
  landing_velocity_ = state.landing_velocity;
  shot_count_ = state.shot_count;
  respawn_count_ = state.respawn_count;
  on_floor_ = state.on_floor;
  health_ = state.health;
  if (state.dead != dead_) {
    dead_ = state.dead;
    SetBodyActive(!dead_);
  }
  prev_input_ = state.prev_input;
  kills_ = state.kills;
  deaths_ = state.deaths;
  local_velocity_ = get_global_basis().xform_inv(state.velocity);
  if (weapon_) {
    weapon_->RestoreState(state.weapon);
  }
}

void FpsCharacter::ApplyState(const PlayerState& state) {
  bool teleported = !has_state_ || state.respawn_count != respawn_count_;
  bool shot = has_state_ && state.shot_count != shot_count_;
  has_state_ = true;
  RestoreState(state);
  if (teleported) {
    OnTeleported();
  }
  // Sustained fire is at most one shot per tick (8 per second at 60
  // ticks), so one shot's effects cover the change.
  if (shot && weapon_) {
    weapon_->PlayShotEffects(AimTransform());
  }
}

void FpsCharacter::OnTeleported() {
  reset_physics_interpolation();
  prev_landing_offset_ = landing_offset_;
  correction_offset_ = Vector3();
  prev_correction_offset_ = Vector3();
  new_correction_ = Vector3();
  look_yaw_ = yaw_;
  look_pitch_ = pitch_;
}

void FpsCharacter::OnCorrected(const Vector3& error) {
  new_correction_ += error;
  if ((correction_offset_ + new_correction_).length() >
      kMaxSmoothedCorrection) {
    // Too far to ease: snap there.
    reset_physics_interpolation();
    correction_offset_ = Vector3();
    prev_correction_offset_ = Vector3();
    new_correction_ = Vector3();
  }
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
  float fraction = static_cast<float>(
      Engine::get_singleton()->get_physics_interpolation_fraction());
  UpdateCamera(get_global_transform_interpolated().origin, landing_offset(),
               prev_correction_offset_.lerp(correction_offset_, fraction),
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
  bool grounded = on_floor_;
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
      HoldFire();
    }
  }
}

void FpsCharacter::Respawn() {
  RespawnAt(spawn_transform_);
}

void FpsCharacter::RespawnAt(const Transform3D& transform) {
  respawn_count_++;
  health_ = kMaxHealth;
  if (dead_) {
    dead_ = false;
    SetBodyActive(true);
  }
  set_global_transform(transform);
  // A teleport: don't draw the body sliding back across the map.
  reset_physics_interpolation();
  set_velocity(Vector3());
  yaw_ = get_rotation().y;
  pitch_ = 0.0f;
  look_yaw_ = yaw_;
  look_pitch_ = 0.0f;
  recoil_ = 0.0f;
  recoil_yaw_ = 0.0f;
  prev_input_ = PlayerInput();
  bob_amount_ = 0.0f;
  landing_offset_ = 0.0f;
  landing_velocity_ = 0.0f;
  prev_landing_offset_ = 0.0f;
  look_delta_ = Vector2();
  look_velocity_ = Vector2();
  local_velocity_ = Vector3();
  correction_offset_ = Vector3();
  prev_correction_offset_ = Vector3();
  new_correction_ = Vector3();
  on_floor_ = false;
  UpdateCamera(get_global_transform().origin, landing_offset_, Vector3(), 0.0f);
  if (weapon_) {
    weapon_->Refill();
  }
}

void FpsCharacter::ApplyLookDelta(const Vector2& delta, float sensitivity) {
  float yaw = -delta.x * sensitivity;
  // Wrapped, so endless turning doesn't lose float precision.
  look_yaw_ = std::remainder(look_yaw_ + yaw, static_cast<float>(Math::TAU));
  float pitch = static_cast<float>(UtilityFunctions::clampf(
      look_pitch_ - delta.y * sensitivity, -kPitchLimit, kPitchLimit));
  // Only the rotation that happened: none against the pitch limit.
  look_delta_ += Vector2(yaw, pitch - look_pitch_);
  look_pitch_ = pitch;
}

bool FpsCharacter::TakeDamage(int amount) {
  if (dead_ || amount <= 0) {
    return false;
  }
  health_ -= amount;
  emit_signal("damaged");
  if (health_ > 0) {
    return false;
  }
  health_ = 0;
  dead_ = true;
  set_velocity(Vector3());
  SetBodyActive(false);
  return true;
}

void FpsCharacter::SetBodyActive(bool active) {
  // Off the player layer: shots and the living pass through.
  set_collision_layer_value(4, active);
  Node3D* mesh = Object::cast_to<Node3D>(get_node_or_null("Mesh"));
  if (mesh) {
    mesh->set_visible(active);
  }
  if (weapon_) {
    weapon_->set_visible(active);
  }
}

void FpsCharacter::HoldFire() {
  fire_held_ = true;
}

void FpsCharacter::UpdateCamera(const Vector3& origin,
                                float landing_dip,
                                const Vector3& correction,
                                float since_tick) {
  if (!camera_) {
    return;
  }
  float decay = std::exp(-kRecoilReturn * since_tick);
  // The local look, not the simulated aim: it reacts to input arriving
  // after the last tick.
  float pitch = static_cast<float>(UtilityFunctions::clampf(
      look_pitch_ + recoil_ * decay, -kPitchLimit, kPitchLimit));
  // The head is lowest when a foot lands (phase 0 and pi).
  float bob = kBobUp * bob_amount_ * -std::cos(2.0f * step_phase_);
  Basis yaw(Vector3(0.0f, 1.0f, 0.0f), look_yaw_);
  camera_->set_global_transform(Transform3D(
      yaw * Basis::from_euler(Vector3(pitch, recoil_yaw_ * decay, 0.0f)),
      origin + correction +
          yaw.xform(eye_offset_ + Vector3(0.0f, bob + landing_dip, 0.0f))));
}

// static
void FpsCharacter::_bind_methods() {
  ClassDB::bind_method(D_METHOD("respawn"), &FpsCharacter::Respawn);
  ClassDB::bind_method(D_METHOD("hold_fire"), &FpsCharacter::HoldFire);
  ClassDB::bind_method(D_METHOD("get_health"), &FpsCharacter::health);
  ClassDB::bind_method(D_METHOD("is_dead"), &FpsCharacter::dead);

  ADD_PROPERTY(PropertyInfo(Variant::INT, "health"), "", "get_health");

  // On the server, when a hit takes health.
  ADD_SIGNAL(MethodInfo("damaged"));
}

void FpsCharacter::OnWeaponFired() {
  recoil_ += kRecoilKick;
  recoil_yaw_ += kRecoilYawKick * SignedNoise(shot_count_++);
}

}  // namespace godot
