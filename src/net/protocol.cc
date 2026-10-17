#include "src/net/protocol.h"

#include <algorithm>
#include <cmath>

#include "src/net/byte_stream.h"
#include "src/net/schema.h"

namespace godot::net {

namespace {

// View ticks: 1/256ths.
constexpr double kViewTickScale = 256.0;

// The entity list: the baseline's again, or listed.
enum Roster : uint8_t {
  kRosterSame = 0,
  kRosterListed = 1,
};

ByteWriter StartMessage(MessageType type) {
  ByteWriter writer;
  writer.U8(kWireVersion);
  writer.U8(static_cast<uint8_t>(type));
  return writer;
}

bool StartReading(ByteReader* reader, MessageType type) {
  return reader->U8() == kWireVersion &&
         reader->U8() == static_cast<uint8_t>(type);
}

// A command's view tick relative to the message's base, in 1/256ths.
int16_t ViewOffset(double view_tick, uint32_t base) {
  double offset = (view_tick - static_cast<double>(base)) * kViewTickScale;
  if (!std::isfinite(offset)) {
    return 0;
  }
  return static_cast<int16_t>(std::clamp(std::lround(offset), -32767L, 32767L));
}

bool SameRoster(const Snapshot& a, const Snapshot& b) {
  if (a.entities.size() != b.entities.size()) {
    return false;
  }
  for (size_t i = 0; i < a.entities.size(); i++) {
    const SnapshotEntity& x = a.entities[i];
    const SnapshotEntity& y = b.entities[i];
    if (x.id != y.id || x.type != y.type || x.owner != y.owner) {
      return false;
    }
  }
  return true;
}

// The entity in the baseline to encode against: the same one, unless it
// changed type or owner (and so what is sent of it).
const SnapshotEntity* BaseOf(const Snapshot* baseline,
                             const SnapshotEntity& entity) {
  const SnapshotEntity* base = baseline ? baseline->Find(entity.id) : nullptr;
  return base && base->type == entity.type && base->owner == entity.owner
             ? base
             : nullptr;
}

}  // namespace

const SnapshotEntity* Snapshot::Find(EntityId id) const {
  auto it = std::lower_bound(
      entities.begin(), entities.end(), id,
      [](const SnapshotEntity& entity, EntityId id) { return entity.id < id; });
  return it != entities.end() && it->id == id ? &*it : nullptr;
}

void QuantizeCommand(TimedCommand* command) {
  Quantize(View::kAll, &command->command);
  // On the wire the view tick is relative to a whole tick, so 1/256ths of
  // a tick whatever the base.
  command->view_tick =
      std::isfinite(command->view_tick)
          ? std::round(command->view_tick * kViewTickScale) / kViewTickScale
          : 0.0;
}

bool PeekMessageType(const PackedByteArray& bytes, MessageType* type) {
  if (bytes.size() < 2 || bytes[0] != kWireVersion) {
    return false;
  }
  uint8_t value = bytes[1];
  if (value < static_cast<uint8_t>(MessageType::kHello) ||
      value > static_cast<uint8_t>(MessageType::kRpc)) {
    return false;
  }
  *type = static_cast<MessageType>(value);
  return true;
}

PackedByteArray EncodeHello(MessageType type, uint32_t schema_hash) {
  ByteWriter writer = StartMessage(type);
  writer.U32(schema_hash);
  return writer.Finish();
}

bool DecodeHello(const PackedByteArray& bytes, uint32_t* schema_hash) {
  ByteReader reader(bytes);
  reader.U8();
  reader.U8();
  *schema_hash = reader.U32();
  return reader.Done();
}

PackedByteArray EncodeInput(const InputMessage& message) {
  ByteWriter writer = StartMessage(MessageType::kInput);
  writer.U32(message.sequence);
  writer.U32(message.snapshot_ack);
  writer.U8(static_cast<uint8_t>(message.commands.size()));
  // View ticks relative to the last command's whole tick.
  uint32_t view_base = 0;
  if (!message.commands.empty() &&
      std::isfinite(message.commands.back().view_tick) &&
      message.commands.back().view_tick >= 0.0) {
    view_base = static_cast<uint32_t>(
        static_cast<int64_t>(message.commands.back().view_tick));
  }
  writer.U32(view_base);
  // Each against the one before (the first against a default command):
  // consecutive commands mostly repeat.
  Box previous;
  for (const TimedCommand& command : message.commands) {
    if (previous.empty()) {
      previous.Reset(*command.command.layout());
    }
    WriteDelta(command.command, previous, View::kAll, &writer);
    writer.I16(ViewOffset(command.view_tick, view_base));
    previous = command.command;
  }
  return writer.Finish();
}

bool DecodeInput(const PackedByteArray& bytes,
                 const Schema& schema,
                 InputMessage* message) {
  ByteReader reader(bytes);
  if (!StartReading(&reader, MessageType::kInput) || !schema.command()) {
    return false;
  }
  message->sequence = reader.U32();
  message->snapshot_ack = reader.U32();
  int count = reader.U8();
  uint32_t view_base = reader.U32();
  if (!reader.ok() || count < 1 || count > kMaxInputCommands) {
    return false;
  }
  message->commands.clear();
  Box previous(*schema.command());
  for (int i = 0; i < count; i++) {
    TimedCommand command;
    command.command = previous;
    if (!ReadDelta(View::kAll, &reader, &command.command)) {
      return false;
    }
    command.view_tick =
        static_cast<double>(view_base) + reader.I16() / kViewTickScale;
    previous = command.command;
    message->commands.push_back(command);
  }
  return reader.Done();
}

PackedByteArray EncodeSnapshot(const Snapshot& message,
                               const Snapshot* baseline,
                               PeerId peer) {
  ByteWriter writer = StartMessage(MessageType::kSnapshot);
  writer.U32(message.tick);
  writer.U32(baseline ? baseline->tick : 0);
  writer.U32(message.ack);
  writer.U8(static_cast<uint8_t>(message.input_buffer));

  if (baseline && SameRoster(message, *baseline)) {
    writer.U8(kRosterSame);
  } else {
    writer.U8(kRosterListed);
    writer.VarUint(message.entities.size());
    EntityId previous = 0;
    for (const SnapshotEntity& entity : message.entities) {
      // Ascending: the gaps are small.
      writer.VarUint(entity.id - previous);
      previous = entity.id;
      writer.U8(entity.type);
      writer.VarUint(static_cast<uint32_t>(entity.owner));
    }
  }
  for (const SnapshotEntity& entity : message.entities) {
    View view = entity.owner == peer ? View::kOwner : View::kOthers;
    const SnapshotEntity* base = BaseOf(baseline, entity);
    if (base) {
      WriteDelta(entity.state, base->state, view, &writer);
    } else {
      WriteDelta(entity.state, Box(*entity.state.layout()), view, &writer);
    }
  }
  return writer.Finish();
}

bool PeekSnapshotBaseline(const PackedByteArray& bytes, uint32_t* baseline) {
  ByteReader reader(bytes);
  if (!StartReading(&reader, MessageType::kSnapshot)) {
    return false;
  }
  reader.U32();
  *baseline = reader.U32();
  return reader.ok();
}

bool DecodeSnapshot(const PackedByteArray& bytes,
                    const Snapshot* baseline,
                    const Schema& schema,
                    PeerId peer,
                    Snapshot* message) {
  ByteReader reader(bytes);
  if (!StartReading(&reader, MessageType::kSnapshot)) {
    return false;
  }
  message->tick = reader.U32();
  message->baseline = reader.U32();
  message->ack = reader.U32();
  message->input_buffer = static_cast<int8_t>(reader.U8());
  if (!reader.ok() || (message->baseline != 0) != (baseline != nullptr) ||
      (baseline && baseline->tick != message->baseline)) {
    return false;
  }

  message->entities.clear();
  uint8_t roster = reader.U8();
  if (roster == kRosterSame && baseline) {
    for (const SnapshotEntity& entity : baseline->entities) {
      message->entities.push_back({entity.id, entity.type, entity.owner, {}});
    }
  } else if (roster == kRosterListed) {
    uint64_t count = reader.VarUint();
    if (!reader.ok() || count > kMaxSnapshotEntities) {
      return false;
    }
    EntityId previous = 0;
    for (uint64_t i = 0; i < count; i++) {
      uint64_t gap = reader.VarUint();
      TypeId type = reader.U8();
      uint64_t owner = reader.VarUint();
      if (!reader.ok() || (gap == 0 && i > 0) || gap > 0xffffffffu ||
          !schema.entity_type(type) || owner > 0x7fffffff) {
        return false;
      }
      previous += static_cast<EntityId>(gap);
      message->entities.push_back(
          {previous, type, static_cast<PeerId>(owner), {}});
    }
  } else {
    return false;
  }
  for (SnapshotEntity& entity : message->entities) {
    const SnapshotEntity* base = BaseOf(baseline, entity);
    if (base) {
      entity.state = base->state;
    } else {
      entity.state.Reset(*schema.entity_type(entity.type));
    }
    View view = entity.owner == peer ? View::kOwner : View::kOthers;
    if (!ReadDelta(view, &reader, &entity.state)) {
      return false;
    }
  }
  return reader.Done();
}

PackedByteArray EncodeRpc(int rpc, const Box& payload) {
  ByteWriter writer = StartMessage(MessageType::kRpc);
  writer.U8(static_cast<uint8_t>(rpc));
  WriteDelta(payload, Box(*payload.layout()), View::kAll, &writer);
  return writer.Finish();
}

bool DecodeRpc(const PackedByteArray& bytes,
               const Schema& schema,
               int* rpc,
               Box* payload) {
  ByteReader reader(bytes);
  if (!StartReading(&reader, MessageType::kRpc)) {
    return false;
  }
  *rpc = reader.U8();
  if (!reader.ok() || static_cast<size_t>(*rpc) >= schema.rpc_count()) {
    return false;
  }
  payload->Reset(*schema.rpc_layout(*rpc));
  return ReadDelta(View::kAll, &reader, payload) && reader.Done();
}

}  // namespace godot::net
