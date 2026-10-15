#ifndef SRC_PLAYER_INPUT_H_
#define SRC_PLAYER_INPUT_H_

#include <godot_cpp/variant/vector2.hpp>

namespace godot {

// One physics tick's command for an FpsCharacter: everything its
// simulation reads from the player. Local devices fill it on a client; a
// server would run the commands it receives.
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
  bool jump = false;
  bool sprint = false;
  bool fire = false;
  bool reload = false;
};

}  // namespace godot

#endif  // SRC_PLAYER_INPUT_H_
