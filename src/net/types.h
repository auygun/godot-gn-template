#ifndef SRC_NET_TYPES_H_
#define SRC_NET_TYPES_H_

#include <cstdint>

namespace godot::net {

// A peer, as SceneMultiplayer numbers them: the server is 1.
using PeerId = int32_t;
inline constexpr PeerId kServerPeerId = 1;
// The owner of an entity no peer controls (it simulates itself on the
// server).
inline constexpr PeerId kNoOwner = 0;

// An entity's id, the same on the server and every client; 0 is none.
using EntityId = uint32_t;
// The index of an entity type, an RPC or the command type in the Schema,
// in the order they were registered (the same on every side).
using TypeId = uint8_t;

// Command sequence numbers and ticks count from 1 and wrap. Whether a is
// after b.
inline bool SequenceNewer(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) > 0;
}

// How a simulation tick runs.
enum class SimulationMode {
  // On the server: the real thing, effects and damage.
  kAuthoritative,
  // On a client, its own entity's command, the first time: effects, but
  // what only the server decides (damage) stays the server's.
  kPredicted,
  // On a client, a command simulated again after a correction: game state
  // only.
  kReplay,
};

// What a command is simulated with, besides the command itself.
struct SimContext {
  float dt = 0.0f;
  SimulationMode mode = SimulationMode::kAuthoritative;
  // The server tick the commanding client was drawing the world at
  // (fractional): what its shots are tested against (NetSession::Raycast).
  double view_tick = 0.0;
};

}  // namespace godot::net

#endif  // SRC_NET_TYPES_H_
