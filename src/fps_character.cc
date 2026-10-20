#include "src/fps_character.h"

#include <cmath>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/collision_object3d.hpp>
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
    if (camera_) {
      // Where the scene puts it relative to the camera (both are children
      // of the body, until they become top-level).
      weapon_->SetFirstPerson(camera_->get_transform().affine_inverse() *
                              weapon_->get_transform());
    }
  }
  if (camera_) {
    camera_->set_as_top_level(true);
    camera_->set_physics_interpolation_mode(PHYSICS_INTERPOLATION_MODE_OFF);
  }
  floor_snap_length_ = get_floor_snap_length();
  spawn_transform_ = get_global_transform();
  state_.yaw = get_rotation().y;
  look_yaw_ = state_.yaw;
  // Until the session draws it.
  view_ = CaptureState();
  if (!local_) {
    // Drawn from the simulation alone: no look, bob or local input.
    set_process(false);
    set_process_unhandled_input(false);
    return;
  }
  Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
}

void FpsCharacter::OnSpawning(net::PeerId owner, bool local) {
  set_name(vformat("Player%d", owner));
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
  command.respawn_count = state_.respawn_count;
  command.jump = held("jump");
  command.sprint = input->is_action_pressed("sprint");
  command.fire = !fire_held_ && held("shoot");
  command.reload = held("reload");
  command.spawn_crate = held("spawn_crate");
  return command;
}

PlayerInput FpsCharacter::IdleInput() const {
  PlayerInput command;
  command.yaw = state_.yaw;
  command.pitch = state_.pitch;
  command.respawn_count = state_.respawn_count;
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
  bool current_look = input.respawn_count == state_.respawn_count;
  if (current_look && std::isfinite(input.yaw)) {
    state_.yaw = std::remainder(input.yaw, static_cast<float>(Math::TAU));
  }
  if (current_look && std::isfinite(input.pitch)) {
    state_.pitch = static_cast<float>(
        UtilityFunctions::clampf(input.pitch, -kPitchLimit, kPitchLimit));
  }
  view_tick_ = context.view_tick;
  // Even when dead: the camera decays it between ticks too.
  float decay = std::exp(-PlayerState::kRecoilReturn * dt);
  state_.recoil *= decay;
  state_.recoil_yaw *= decay;
  if (state_.dead) {
    // Lying where it died until the server respawns it; what the player
    // presses meanwhile mustn't count as a press after.
    state_.prev_input = PlayerInput();
    return;
  }
  bool jump_pressed = input.jump && !state_.prev_input.jump;
  bool reload_pressed = input.reload && !state_.prev_input.reload;
  bool spawn_crate_pressed =
      input.spawn_crate && !state_.prev_input.spawn_crate;
  state_.prev_input = input;

  set_rotation(Vector3(0.0f, state_.yaw, 0.0f));

  bool was_on_floor = state_.on_floor;
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
  // by the state's on_floor instead keeps the result a function of the game
  // state (the last call may be a later tick, when replaying).
  set_floor_snap_length(was_on_floor ? floor_snap_length_ : 0.0f);
  move_and_slide();

  state_.on_floor = is_on_floor();
  bool landed = !was_on_floor && state_.on_floor;
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

  // The server (Main) spawns it.
  if (spawn_crate_pressed && mode == net::SimulationMode::kAuthoritative) {
    emit_signal("crate_requested");
  }
}

Transform3D FpsCharacter::GetAimTransform() const {
  float pitch = static_cast<float>(UtilityFunctions::clampf(
      state_.pitch + state_.recoil, -kPitchLimit, kPitchLimit));
  Basis yaw(Vector3(0.0f, 1.0f, 0.0f), state_.yaw);
  return Transform3D(
      yaw * Basis::from_euler(Vector3(pitch, state_.recoil_yaw, 0.0f)),
      get_global_position() +
          yaw.xform(eye_offset_ + Vector3(0.0f, state_.landing_offset, 0.0f)));
}

PlayerState FpsCharacter::CaptureState() const {
  PlayerState state = state_;
  state.position = get_global_position();
  state.velocity = get_velocity();
  if (weapon_) {
    state.weapon = weapon_->CaptureState();
  }
  return state;
}

void FpsCharacter::RestoreState(const PlayerState& state) {
  bool was_dead = state_.dead;
  state_ = state;
  set_global_transform(
      Transform3D(Basis(Vector3(0.0f, 1.0f, 0.0f), state.yaw), state.position));
  set_velocity(state.velocity);
  if (state.dead != was_dead) {
    SetBodyActive(!state.dead);
  }
  local_velocity_ = get_global_basis().xform_inv(state.velocity);
  if (weapon_) {
    weapon_->RestoreState(state.weapon);
  }
}

void FpsCharacter::ApplyState(const PlayerState& state) {
  bool teleported = !has_state_ || state.respawn_count != state_.respawn_count;
  bool shot = has_state_ && state.shot_count != state_.shot_count;
  has_state_ = true;
  RestoreState(state);
  if (teleported) {
    OnTeleported();
  }
  // Sustained fire is at most one shot per tick (8 per second at 60
  // ticks), so one shot's effects cover the change.
  if (shot && weapon_) {
    weapon_->PlayShotEffects(GetAimTransform());
  }
}

void FpsCharacter::OnTeleported() {
  reset_physics_interpolation();
  look_yaw_ = state_.yaw;
  look_pitch_ = state_.pitch;
}

void FpsCharacter::Draw(const PlayerState& view) {
  view_ = view;
}

void FpsCharacter::_process(double delta) {
  float dt = static_cast<float>(delta);
  if (dt > 0.0f) {
    look_velocity_ = look_delta_ / dt;
    look_delta_ = Vector2();
    UpdateBob(dt);
  }
  // The NetSession's _process (which runs first) has drawn this frame.
  UpdateCamera();
}

void FpsCharacter::UpdateLandingDip(float dt, float landing_speed) {
  if (landing_speed > kMinLandingSpeed) {
    state_.landing_velocity -=
        kLandingImpulse * static_cast<float>(UtilityFunctions::minf(
                              landing_speed, kMaxLandingSpeed));
  }
  // A critically damped spring back to rest, solved exactly.
  float w = kLandingSpring;
  float c = state_.landing_velocity + w * state_.landing_offset;
  float e = std::exp(-w * dt);
  state_.landing_offset = (state_.landing_offset + c * dt) * e;
  state_.landing_velocity = (state_.landing_velocity - w * c * dt) * e;
}

void FpsCharacter::UpdateBob(float dt) {
  bool grounded = state_.on_floor;
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
  state_.respawn_count++;
  state_.health = kMaxHealth;
  if (state_.dead) {
    state_.dead = false;
    SetBodyActive(true);
  }
  set_global_transform(transform);
  // A teleport: don't draw the body sliding back across the map.
  reset_physics_interpolation();
  set_velocity(Vector3());
  state_.yaw = get_rotation().y;
  state_.pitch = 0.0f;
  look_yaw_ = state_.yaw;
  look_pitch_ = 0.0f;
  state_.recoil = 0.0f;
  state_.recoil_yaw = 0.0f;
  state_.prev_input = PlayerInput();
  bob_amount_ = 0.0f;
  state_.landing_offset = 0.0f;
  state_.landing_velocity = 0.0f;
  look_delta_ = Vector2();
  look_velocity_ = Vector2();
  local_velocity_ = Vector3();
  state_.on_floor = false;
  view_ = CaptureState();
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
  if (state_.dead || amount <= 0) {
    return false;
  }
  // The state's health is a byte: subtract without wrapping.
  int health = state_.health - amount;
  state_.health = static_cast<uint8_t>(health > 0 ? health : 0);
  emit_signal("damaged");
  if (health > 0) {
    return false;
  }
  state_.dead = true;
  set_velocity(Vector3());
  SetBodyActive(false);
  return true;
}

void FpsCharacter::SetBodyActive(bool active) {
  // Off the player layer: shots and the living pass through.
  set_collision_layer_value(4, active);
  // Turn the blocker off too, so crates don't rest on a head nobody can see.
  CollisionObject3D* prop_blocker =
      Object::cast_to<CollisionObject3D>(get_node_or_null("PropBlocker"));
  if (prop_blocker) {
    prop_blocker->set_collision_layer(active ? kPropBlockerLayer : 0);
  }
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

Transform3D FpsCharacter::GetCameraTransform() const {
  // The local look, not the simulated aim: it reacts to input arriving
  // after the last tick.
  float pitch = static_cast<float>(UtilityFunctions::clampf(
      look_pitch_ + view_.recoil, -kPitchLimit, kPitchLimit));
  // The head is lowest when a foot lands (phase 0 and pi).
  float bob = kBobUp * bob_amount_ * -std::cos(2.0f * step_phase_);
  Basis yaw(Vector3(0.0f, 1.0f, 0.0f), look_yaw_);
  return Transform3D(
      yaw * Basis::from_euler(Vector3(pitch, view_.recoil_yaw, 0.0f)),
      view_.position +
          yaw.xform(eye_offset_ +
                    Vector3(0.0f, bob + view_.landing_offset, 0.0f)));
}

void FpsCharacter::UpdateCamera() {
  if (camera_) {
    camera_->set_global_transform(GetCameraTransform());
  }
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
  // On the server, when the player presses "spawn_crate".
  ADD_SIGNAL(MethodInfo("crate_requested"));
}

void FpsCharacter::OnWeaponFired() {
  state_.recoil += kRecoilKick;
  state_.recoil_yaw += kRecoilYawKick * SignedNoise(state_.shot_count++);
}

}  // namespace godot
