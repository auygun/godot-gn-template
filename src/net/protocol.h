#ifndef SRC_NET_PROTOCOL_H_
#define SRC_NET_PROTOCOL_H_

#include <cstdint>
#include <vector>

#include <godot_cpp/variant/packed_byte_array.hpp>

#include "src/net/replicate.h"
#include "src/net/types.h"

namespace godot::net {

class Schema;

// The messages between the server and its clients, sent with
// SceneMultiplayer::send_bytes (and compressed by ENet). Every packet
// starts with kWireVersion and a MessageType. Decoding checks every length
// and count: a packet that doesn't decode is dropped (a server mustn't
// trust its clients).
//
// What a game sends is declared in its Schema; kWireVersion covers only
// the framework's own format, and peers compare the Schema's hash when
// they connect (kHello).
//
// Snapshots are delta encoded: against a snapshot the client acknowledged
// receiving (its baseline), only the fields that changed are sent. Lost
// snapshots don't break later ones, as the baseline is always one the
// client has.

inline constexpr uint8_t kWireVersion = 1;

enum class MessageType : uint8_t {
  // Client to server, reliable, on connecting: the Schema's hash.
  kHello = 1,
  // Server to client, reliable: the hashes differ (the server's).
  kReject = 2,
  // Client to server, unreliable, every tick: the latest commands.
  kInput = 3,
  // Server to client, unreliable: the world, as a delta.
  kSnapshot = 4,
  // Either way, reliable: a game's RPC.
  kRpc = 5,
};

// Each input message repeats this many of the latest commands, so a lost
// packet loses none.
inline constexpr int kMaxInputCommands = 4;
inline constexpr int kMaxSnapshotEntities = 1024;

// A command and the server tick its client was drawing (fractional, in
// 1/256ths on the wire).
struct TimedCommand {
  Box command;
  double view_tick = 0.0;
};

// The command (and view tick) as the server will decode them. A client
// predicts with this, not the original, or its predictions would drift
// from the server's.
void QuantizeCommand(TimedCommand* command);

struct InputMessage {
  // The sequence number of the last command; the ones before it count
  // down.
  uint32_t sequence = 0;
  // The newest snapshot tick the client has decoded (0: none): the server
  // encodes the next snapshots against it.
  uint32_t snapshot_ack = 0;
  // Oldest first, at most kMaxInputCommands. Quantized (QuantizeCommand).
  std::vector<TimedCommand> commands;
};

struct SnapshotEntity {
  EntityId id = 0;
  TypeId type = 0;
  PeerId owner = kNoOwner;
  // As its receiver sees it: the owner's view of its own entities, the
  // others' of the rest (decoded); the full state (on the server).
  Box state;
};

struct Snapshot {
  // Ticks the server has simulated.
  uint32_t tick = 0;
  // The snapshot this one was encoded against (0: none, a full snapshot).
  uint32_t baseline = 0;
  // The last of this client's commands the server simulated (0: none).
  uint32_t ack = 0;
  // How many ticks ahead of being simulated this client's commands arrive:
  // the newest received minus ack. Negative when they arrive too late (the
  // server repeated the last command in their place).
  int8_t input_buffer = 0;
  // By id, ascending.
  std::vector<SnapshotEntity> entities;

  const SnapshotEntity* Find(EntityId id) const;
};

// The type of a message, or false if it isn't one.
bool PeekMessageType(const PackedByteArray& bytes, MessageType* type);

PackedByteArray EncodeHello(MessageType type, uint32_t schema_hash);
bool DecodeHello(const PackedByteArray& bytes, uint32_t* schema_hash);

PackedByteArray EncodeInput(const InputMessage& message);
bool DecodeInput(const PackedByteArray& bytes,
                 const Schema& schema,
                 InputMessage* message);

// message for peer, against baseline: null for a full snapshot, else an
// earlier snapshot the client acknowledged.
PackedByteArray EncodeSnapshot(const Snapshot& message,
                               const Snapshot* baseline,
                               PeerId peer);
// The tick a snapshot was encoded against (0: none), so the caller can
// find that snapshot; false if bytes isn't a snapshot.
bool PeekSnapshotBaseline(const PackedByteArray& bytes, uint32_t* baseline);
// baseline: the decoded snapshot of that tick (null when it is 0). peer:
// this client's.
bool DecodeSnapshot(const PackedByteArray& bytes,
                    const Snapshot* baseline,
                    const Schema& schema,
                    PeerId peer,
                    Snapshot* message);

PackedByteArray EncodeRpc(int rpc, const Box& payload);
bool DecodeRpc(const PackedByteArray& bytes,
               const Schema& schema,
               int* rpc,
               Box* payload);

}  // namespace godot::net

#endif  // SRC_NET_PROTOCOL_H_
