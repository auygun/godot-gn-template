#ifndef SRC_PLAYER_STATE_H_
#define SRC_PLAYER_STATE_H_

#include <cstdint>

#include <godot_cpp/variant/vector3.hpp>

#include "src/player_input.h"

namespace godot {

// How a simulation tick runs.
enum class SimulationMode {
  // On the server: the real thing, effects and damage.
  kAuthoritative,
  // On a client, its own player's command, the first time: effects, but
  // damage is the server's.
  kPredicted,
  // On a client, a command simulated again after a correction: game state
  // only.
  kReplay,
};

// A Weapon's game state: what Weapon::Tick reads and writes.
struct WeaponState {
  int ammo = 0;
  int reserve_ammo = 0;
  bool reloading = false;
  float reload_timer = 0.0f;
  float fire_cooldown = 0.0f;
};

// An FpsCharacter's game state: everything Simulate reads and writes. The
// server sends it to clients, which draw it; restoring it (and replaying
// commands) rewinds the simulation.
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
  // The last command, to find button presses (only the buttons matter).
  PlayerInput prev_input;
  WeaponState weapon;
};

}  // namespace godot

#endif  // SRC_PLAYER_STATE_H_
