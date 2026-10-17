#ifndef SRC_PLAYER_INPUT_H_
#define SRC_PLAYER_INPUT_H_

#include <cstdint>

#include <godot_cpp/variant/vector2.hpp>

#include "src/net/replicate.h"

namespace godot {

// One physics tick's command for an FpsCharacter: everything its
// simulation reads from the player. Local devices fill it on its owner's
// machine; the server runs the commands it receives.
//
// Buttons are held states, not presses: the simulation finds presses by
// comparing with the previous command, so a repeated command (e.g. resent
// after packet loss) can't press twice. Look angles are absolute, so a
// lost command loses no turning.
struct PlayerInput {
  // x: right, y: back; length at most 1.
  Vector2 move;
  // Look angles in radians: yaw around +Y, pitch up from the horizon.
  float yaw = 0.0f;
  float pitch = 0.0f;
  // The respawn the look angles are from (FpsCharacter counts them): the
  // simulation ignores the look of commands from before the latest
  // respawn, which would undo the spawn's facing.
  uint8_t respawn_count = 0;
  bool jump = false;
  bool sprint = false;
  bool fire = false;
  bool reload = false;

  template <class V>
  void Replicate(V& v) {
    v(move, net::Unit());
    v(yaw, net::Angle());
    v(pitch, net::Pitch());
    v(respawn_count, net::Exact());
    v(jump, net::Exact());
    v(sprint, net::Exact());
    v(fire, net::Exact());
    v(reload, net::Exact());
  }
};

}  // namespace godot

#endif  // SRC_PLAYER_INPUT_H_
