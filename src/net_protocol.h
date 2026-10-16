#ifndef SRC_NET_PROTOCOL_H_
#define SRC_NET_PROTOCOL_H_

#include <cstdint>
#include <vector>

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "src/player_input.h"
#include "src/player_state.h"

namespace godot {

// The messages between the server and its clients, sent with
// SceneMultiplayer::send_bytes. Every packet starts with kProtocolVersion
// and a MessageType. Decoding checks every length and count: a packet that
// doesn't decode is dropped (a server mustn't trust its clients).
//
// Everything is little-endian (every supported platform is), floats raw.

inline constexpr uint8_t kProtocolVersion = 2;
inline constexpr int kDefaultPort = 24680;

enum class MessageType : uint8_t {
  // Client to server, unreliable, every tick: the latest commands.
  kInput = 1,
  // Server to client, unreliable, every tick: the game state.
  kSnapshot = 2,
  // Server to client, reliable: something the client's player did.
  kEvent = 3,
};

// Each input message repeats this many of the latest commands, so a lost
// packet loses none.
inline constexpr int kMaxInputCommands = 4;
// Snapshots carry at most this many of each.
inline constexpr int kMaxSnapshotPlayers = 64;
inline constexpr int kMaxSnapshotTargets = 256;

// Commands are numbered from 1, one per client tick, wrapping.
// Whether sequence a is after b.
inline bool SequenceNewer(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) > 0;
}

struct InputMessage {
  // The sequence number of the last command; the ones before it count
  // down.
  uint32_t sequence = 0;
  // Oldest first, at most kMaxInputCommands.
  std::vector<PlayerInput> commands;
};

struct SnapshotPlayer {
  int32_t peer_id = 0;
  PlayerState state;
};

struct SnapshotTarget {
  Vector3 position;
  bool dead = false;
};

struct SnapshotMessage {
  // Ticks the server has simulated.
  uint32_t tick = 0;
  // The last of this client's commands the server simulated (0: none).
  uint32_t ack = 0;
  // How many ticks ahead of being simulated this client's commands arrive:
  // the newest received minus ack. Negative when they arrive too late (the
  // server repeated the last command in their place).
  int8_t input_buffer = 0;
  std::vector<SnapshotPlayer> players;
  // In the order of the scene's Targets, the same on both sides.
  std::vector<SnapshotTarget> targets;
};

enum class EventType : uint8_t {
  // The client's player's shot hit something.
  kHit = 1,
  // ...and killed it.
  kKill = 2,
};

struct EventMessage {
  EventType type = EventType::kHit;
};

// The type of a message, or false if it isn't one of this protocol
// version's.
bool PeekMessageType(const PackedByteArray& bytes, MessageType* type);

PackedByteArray EncodeInput(const InputMessage& message);
PackedByteArray EncodeSnapshot(const SnapshotMessage& message);
PackedByteArray EncodeEvent(const EventMessage& message);

// False (and *message unspecified) if bytes isn't a whole message of the
// type.
bool DecodeInput(const PackedByteArray& bytes, InputMessage* message);
bool DecodeSnapshot(const PackedByteArray& bytes, SnapshotMessage* message);
bool DecodeEvent(const PackedByteArray& bytes, EventMessage* message);

}  // namespace godot

#endif  // SRC_NET_PROTOCOL_H_
