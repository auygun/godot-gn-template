#include "src/net_protocol.h"

#include <cstring>

namespace godot {

namespace {

// PlayerInput's buttons, and PlayerState's flags after them.
enum Flags : uint8_t {
  kJump = 1 << 0,
  kSprint = 1 << 1,
  kFire = 1 << 2,
  kReload = 1 << 3,
  kOnFloor = 1 << 4,
  kReloading = 1 << 5,
};

class ByteWriter {
 public:
  explicit ByteWriter(MessageType type) {
    U8(kProtocolVersion);
    U8(static_cast<uint8_t>(type));
  }

  void U8(uint8_t value) { bytes_.push_back(value); }
  void U16(uint16_t value) { Raw(&value, sizeof(value)); }
  void U32(uint32_t value) { Raw(&value, sizeof(value)); }
  void Float(float value) { Raw(&value, sizeof(value)); }
  void Vector(const Vector3& value) {
    Float(value.x);
    Float(value.y);
    Float(value.z);
  }

  PackedByteArray Finish() const {
    PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(bytes_.size()));
    std::memcpy(bytes.ptrw(), bytes_.data(), bytes_.size());
    return bytes;
  }

 private:
  void Raw(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bytes_.insert(bytes_.end(), p, p + size);
  }

  std::vector<uint8_t> bytes_;
};

// Reads past the end return 0 and fail the reader.
class ByteReader {
 public:
  ByteReader(const PackedByteArray& bytes, MessageType type)
      : data_(bytes.ptr()), size_(static_cast<size_t>(bytes.size())) {
    ok_ = U8() == kProtocolVersion && U8() == static_cast<uint8_t>(type);
  }

  uint8_t U8() {
    uint8_t value = 0;
    Raw(&value, sizeof(value));
    return value;
  }
  uint16_t U16() {
    uint16_t value = 0;
    Raw(&value, sizeof(value));
    return value;
  }
  uint32_t U32() {
    uint32_t value = 0;
    Raw(&value, sizeof(value));
    return value;
  }
  float Float() {
    float value = 0.0f;
    Raw(&value, sizeof(value));
    return value;
  }
  Vector3 Vector() {
    float x = Float();
    float y = Float();
    float z = Float();
    return Vector3(x, y, z);
  }

  // Whether every read so far was in bounds. Done() also checks that
  // nothing is left over.
  bool ok() const { return ok_; }
  bool Done() const { return ok_ && pos_ == size_; }

 private:
  void Raw(void* out, size_t size) {
    if (!ok_ || size > size_ - pos_) {
      ok_ = false;
      return;
    }
    std::memcpy(out, data_ + pos_, size);
    pos_ += size;
  }

  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  bool ok_ = true;
};

uint8_t ButtonFlags(const PlayerInput& input) {
  return (input.jump ? kJump : 0) | (input.sprint ? kSprint : 0) |
         (input.fire ? kFire : 0) | (input.reload ? kReload : 0);
}

void SetButtons(uint8_t flags, PlayerInput* input) {
  input->jump = flags & kJump;
  input->sprint = flags & kSprint;
  input->fire = flags & kFire;
  input->reload = flags & kReload;
}

void WriteCommand(const PlayerInput& command, ByteWriter* writer) {
  writer->Float(command.move.x);
  writer->Float(command.move.y);
  writer->Float(command.yaw);
  writer->Float(command.pitch);
  writer->U8(command.respawn_count);
  writer->U8(ButtonFlags(command));
}

PlayerInput ReadCommand(ByteReader* reader) {
  PlayerInput command;
  command.move.x = reader->Float();
  command.move.y = reader->Float();
  command.yaw = reader->Float();
  command.pitch = reader->Float();
  command.respawn_count = reader->U8();
  SetButtons(reader->U8(), &command);
  return command;
}

void WriteState(const PlayerState& state, ByteWriter* writer) {
  writer->Vector(state.position);
  writer->Vector(state.velocity);
  writer->Float(state.yaw);
  writer->Float(state.pitch);
  writer->Float(state.recoil);
  writer->Float(state.recoil_yaw);
  writer->Float(state.landing_offset);
  writer->Float(state.landing_velocity);
  writer->U32(state.shot_count);
  writer->U8(state.respawn_count);
  writer->U8(ButtonFlags(state.prev_input) | (state.on_floor ? kOnFloor : 0) |
             (state.weapon.reloading ? kReloading : 0));
  writer->U16(static_cast<uint16_t>(state.weapon.ammo));
  writer->U16(static_cast<uint16_t>(state.weapon.reserve_ammo));
  writer->Float(state.weapon.reload_timer);
  writer->Float(state.weapon.fire_cooldown);
}

PlayerState ReadState(ByteReader* reader) {
  PlayerState state;
  state.position = reader->Vector();
  state.velocity = reader->Vector();
  state.yaw = reader->Float();
  state.pitch = reader->Float();
  state.recoil = reader->Float();
  state.recoil_yaw = reader->Float();
  state.landing_offset = reader->Float();
  state.landing_velocity = reader->Float();
  state.shot_count = reader->U32();
  state.respawn_count = reader->U8();
  uint8_t flags = reader->U8();
  SetButtons(flags, &state.prev_input);
  state.on_floor = flags & kOnFloor;
  state.weapon.reloading = flags & kReloading;
  state.weapon.ammo = reader->U16();
  state.weapon.reserve_ammo = reader->U16();
  state.weapon.reload_timer = reader->Float();
  state.weapon.fire_cooldown = reader->Float();
  return state;
}

}  // namespace

bool PeekMessageType(const PackedByteArray& bytes, MessageType* type) {
  if (bytes.size() < 2 || bytes[0] != kProtocolVersion) {
    return false;
  }
  uint8_t value = bytes[1];
  if (value < static_cast<uint8_t>(MessageType::kInput) ||
      value > static_cast<uint8_t>(MessageType::kEvent)) {
    return false;
  }
  *type = static_cast<MessageType>(value);
  return true;
}

PackedByteArray EncodeInput(const InputMessage& message) {
  ByteWriter writer(MessageType::kInput);
  writer.U32(message.sequence);
  writer.U8(static_cast<uint8_t>(message.commands.size()));
  for (const PlayerInput& command : message.commands) {
    WriteCommand(command, &writer);
  }
  return writer.Finish();
}

bool DecodeInput(const PackedByteArray& bytes, InputMessage* message) {
  ByteReader reader(bytes, MessageType::kInput);
  message->sequence = reader.U32();
  int count = reader.U8();
  if (!reader.ok() || count < 1 || count > kMaxInputCommands) {
    return false;
  }
  message->commands.clear();
  for (int i = 0; i < count; i++) {
    message->commands.push_back(ReadCommand(&reader));
  }
  return reader.Done();
}

PackedByteArray EncodeSnapshot(const SnapshotMessage& message) {
  ByteWriter writer(MessageType::kSnapshot);
  writer.U32(message.tick);
  writer.U32(message.ack);
  writer.U8(static_cast<uint8_t>(message.players.size()));
  for (const SnapshotPlayer& player : message.players) {
    writer.U32(static_cast<uint32_t>(player.peer_id));
    WriteState(player.state, &writer);
  }
  writer.U16(static_cast<uint16_t>(message.targets.size()));
  for (const SnapshotTarget& target : message.targets) {
    writer.Vector(target.position);
    writer.U8(target.dead ? 1 : 0);
  }
  return writer.Finish();
}

bool DecodeSnapshot(const PackedByteArray& bytes, SnapshotMessage* message) {
  ByteReader reader(bytes, MessageType::kSnapshot);
  message->tick = reader.U32();
  message->ack = reader.U32();
  int player_count = reader.U8();
  if (!reader.ok() || player_count > kMaxSnapshotPlayers) {
    return false;
  }
  message->players.clear();
  for (int i = 0; i < player_count && reader.ok(); i++) {
    SnapshotPlayer player;
    player.peer_id = static_cast<int32_t>(reader.U32());
    player.state = ReadState(&reader);
    message->players.push_back(player);
  }
  int target_count = reader.U16();
  if (!reader.ok() || target_count > kMaxSnapshotTargets) {
    return false;
  }
  message->targets.clear();
  for (int i = 0; i < target_count && reader.ok(); i++) {
    SnapshotTarget target;
    target.position = reader.Vector();
    target.dead = reader.U8() != 0;
    message->targets.push_back(target);
  }
  return reader.Done();
}

PackedByteArray EncodeEvent(const EventMessage& message) {
  ByteWriter writer(MessageType::kEvent);
  writer.U8(static_cast<uint8_t>(message.type));
  return writer.Finish();
}

bool DecodeEvent(const PackedByteArray& bytes, EventMessage* message) {
  ByteReader reader(bytes, MessageType::kEvent);
  uint8_t type = reader.U8();
  if (type != static_cast<uint8_t>(EventType::kHit) &&
      type != static_cast<uint8_t>(EventType::kKill)) {
    return false;
  }
  message->type = static_cast<EventType>(type);
  return reader.Done();
}

}  // namespace godot
