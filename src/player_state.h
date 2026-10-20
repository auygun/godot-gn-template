#ifndef SRC_PLAYER_STATE_H_
#define SRC_PLAYER_STATE_H_

#include <cstdint>

#include <godot_cpp/variant/vector3.hpp>

#include "src/net/replicate.h"
#include "src/net/types.h"
#include "src/player_input.h"

namespace godot {

// A Weapon's game state: what Weapon::Tick reads and writes.
struct WeaponState {
  int32_t ammo = 0;
  int32_t reserve_ammo = 0;
  bool reloading = false;
  float reload_timer = 0.0f;
  float fire_cooldown = 0.0f;

  // The owner's only: others see the shots (PlayerState::shot_count).
  template <class V>
  void Replicate(V& v) {
    v(ammo, net::Exact(), net::kOwner | net::kPredicted);
    v(reserve_ammo, net::Exact(), net::kOwner | net::kPredicted);
    v(reloading, net::Exact(), net::kOwner | net::kPredicted);
    v(reload_timer, net::Exact(), net::kOwner);
    v(fire_cooldown, net::Exact(), net::kOwner);
  }
};

// An FpsCharacter's game state: everything Simulate reads and writes. The
// server sends it to clients: its owner gets all of it exact (it predicts
// from it), the others what they draw, quantized. Restoring it (and
// replaying commands) rewinds the simulation.
struct PlayerState {
  Vector3 position;
  Vector3 velocity;
  float yaw = 0.0f;
  float pitch = 0.0f;
  float recoil = 0.0f;
  float recoil_yaw = 0.0f;
  float landing_offset = 0.0f;
  float landing_velocity = 0.0f;
  // Shots fired; a client plays the shot effects when it goes up.
  uint32_t shot_count = 0;
  // Respawns (wrapping); a client snaps its look when it changes.
  uint8_t respawn_count = 0;
  bool on_floor = false;
  // Only the server changes these (damage); a dead player's commands do
  // nothing until it respawns.
  uint8_t health = 100;
  bool dead = false;
  // The last command, to find button presses (only the buttons pressed
  // rather than held matter).
  PlayerInput prev_input;
  WeaponState weapon;
  // This match's score; only the server changes it.
  uint16_t kills = 0;
  uint16_t deaths = 0;

  // How fast Simulate returns the recoil, 1/s.
  static constexpr float kRecoilReturn = 6.0f;
  // How fast a correction of the view eases out, 1/s; a position more
  // than kMaxSmoothedCorrection off snaps.
  static constexpr float kCorrectionRate = 15.0f;
  static constexpr float kMaxSmoothedCorrection = 1.0f;  // meters

  template <class V>
  void Replicate(V& v) {
    using namespace net;
    // What the camera draws (FpsCharacter::Draw): the body between the
    // last two ticks, the recoil decaying on from the last, as Simulate
    // decays it. Yaw and pitch aren't: the camera looks along the latest
    // look input.
    v(position, Position(), kAll | kPredicted | kBlend, Lerp,
      EaseExp<kCorrectionRate>, kMaxSmoothedCorrection);
    v(velocity, Exact(1e-2f), kOwner | kPredicted);
    v(yaw, Angle(), kAll | kPredicted | kBlend);
    v(pitch, Pitch(), kAll | kPredicted | kBlend);
    v(recoil, Exact(), kOwner | kPredicted, Decay<kRecoilReturn>,
      EaseExp<kCorrectionRate>);
    v(recoil_yaw, Exact(), kOwner | kPredicted, Decay<kRecoilReturn>,
      EaseExp<kCorrectionRate>);
    v(landing_offset, Exact(), kOwner | kPredicted, Lerp,
      EaseExp<kCorrectionRate>);
    v(landing_velocity, Exact(), kOwner);
    // Others play a shot's effects when it changes.
    v(shot_count, LowByte(), kAll | kPredicted);
    v(respawn_count, Exact(), kAll | kServer | kTeleport);
    v(on_floor, Exact(), kAll | kPredicted);
    v(health, Exact(), kOwner | kServer);
    v(dead, Exact(), kAll | kServer);
    v(prev_input.jump, Exact(), kOwner);
    v(prev_input.reload, Exact(), kOwner);
    v(prev_input.spawn_crate, Exact(), kOwner);
    weapon.Replicate(v);
    v(kills, Exact(), kAll | kServer);
    v(deaths, Exact(), kAll | kServer);
  }
};

// A Target's state, for clients to draw (the server simulates it).
struct TargetState {
  Vector3 position;
  bool dead = false;

  template <class V>
  void Replicate(V& v) {
    v(position, net::Position(), net::kAll | net::kBlend);
    v(dead, net::Exact(), net::kAll | net::kTeleport);
  }
};

}  // namespace godot

#endif  // SRC_PLAYER_STATE_H_
