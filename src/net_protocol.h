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
// SceneMultiplayer::send_bytes (and compressed by ENet). Every packet
// starts with kProtocolVersion and a MessageType. Decoding checks every
// length and count: a packet that doesn't decode is dropped (a server
// mustn't trust its clients).
//
// Everything is little-endian (every supported platform is). What is only
// drawn is quantized (positions to 2 mm, angles to 16 bits); what a client
// predicts from is exact (its own player's state) or quantized before the
// client uses it (its commands, QuantizeCommand), so both sides compute
// with the same values.
//
// Snapshots are delta encoded: against a snapshot the client acknowledged
// receiving (its baseline), only what changed is sent. Lost snapshots
// don't break later ones, as the baseline is always one the client has.

inline constexpr uint8_t kProtocolVersion = 5;
inline constexpr int kDefaultPort = 24680;

enum class MessageType : uint8_t {
  // Client to server, unreliable, every tick: the latest commands.
  kInput = 1,
  // Server to client, unreliable: the game state, as a delta.
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

// The command as the server will decode it. A client predicts with this,
// not the original, or its predictions would drift from the server's.
PlayerInput QuantizeCommand(const PlayerInput& command);

struct InputMessage {
  // The sequence number of the last command; the ones before it count
  // down.
  uint32_t sequence = 0;
  // The newest snapshot tick the client has decoded (0: none): the server
  // encodes the next snapshots against it.
  uint32_t snapshot_ack = 0;
  // Oldest first, at most kMaxInputCommands. Quantized (QuantizeCommand).
  std::vector<PlayerInput> commands;
};

// Another player as a client draws it: its position, look and what
// triggers effects, quantized.
struct RemoteView {
  Vector3 position;
  float yaw = 0.0f;
  float pitch = 0.0f;
  bool dead = false;
  bool on_floor = false;
  // The low bytes of the counters: a change plays a shot or teleports.
  uint8_t shot_count = 0;
  uint8_t respawn_count = 0;

  // The full state's equivalent, as far as it goes (for
  // FpsCharacter::ApplyState).
  PlayerState ToState() const;
  bool operator==(const RemoteView& other) const = default;
};

// state as a RemoteView, quantized.
RemoteView MakeRemoteView(const PlayerState& state);

struct SnapshotPlayer {
  int32_t peer_id = 0;
  // The full state, sent only to the player's own client. In a decoded
  // snapshot, the other players' is remote.ToState().
  PlayerState state;
  RemoteView remote;
  // This match's score.
  uint16_t kills = 0;
  uint16_t deaths = 0;
};

struct SnapshotTarget {
  // Quantized.
  Vector3 position;
  bool dead = false;
  bool operator==(const SnapshotTarget& other) const = default;
};

struct SnapshotMessage {
  // Ticks the server has simulated.
  uint32_t tick = 0;
  // The snapshot this one was encoded against (0: none, a full snapshot).
  uint32_t baseline = 0;
  // The client this is for: its player's full state is sent, the others'
  // remote views.
  int32_t local_peer_id = 0;
  // The last of this client's commands the server simulated (0: none).
  uint32_t ack = 0;
  // How many ticks ahead of being simulated this client's commands arrive:
  // the newest received minus ack. Negative when they arrive too late (the
  // server repeated the last command in their place).
  int8_t input_buffer = 0;
  // By peer id, ascending.
  std::vector<SnapshotPlayer> players;
  // In the order of the scene's Targets, the same on both sides.
  std::vector<SnapshotTarget> targets;
};

// Quantize what the wire carries quantized: the players' remote views
// (from their states) and the targets. The server stores and encodes this.
void QuantizeSnapshot(SnapshotMessage* snapshot);

enum class EventType : uint8_t {
  // The client's player's shot hit something.
  kHit = 1,
  // ...and killed another player.
  kKill = 2,
  // The client's player was hit.
  kDamaged = 3,
};

struct EventMessage {
  EventType type = EventType::kHit;
};

// The type of a message, or false if it isn't one of this protocol
// version's.
bool PeekMessageType(const PackedByteArray& bytes, MessageType* type);

PackedByteArray EncodeInput(const InputMessage& message);
// message (quantized) for its local_peer_id, against baseline: null for a
// full snapshot, else an earlier snapshot the client acknowledged.
PackedByteArray EncodeSnapshot(const SnapshotMessage& message,
                               const SnapshotMessage* baseline);
PackedByteArray EncodeEvent(const EventMessage& message);

// False (and *message unspecified) if bytes isn't a whole message of the
// type.
bool DecodeInput(const PackedByteArray& bytes, InputMessage* message);
// The tick a snapshot was encoded against (0: none), so the caller can
// find that snapshot; false if bytes isn't a snapshot.
bool PeekSnapshotBaseline(const PackedByteArray& bytes, uint32_t* baseline);
// baseline: the decoded snapshot of that tick (null when it is 0).
bool DecodeSnapshot(const PackedByteArray& bytes,
                    const SnapshotMessage* baseline,
                    SnapshotMessage* message);
bool DecodeEvent(const PackedByteArray& bytes, EventMessage* message);

}  // namespace godot

#endif  // SRC_NET_PROTOCOL_H_
