#include "src/net_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <godot_cpp/core/math_defs.hpp>

namespace godot {

namespace {

// Positions: 2 mm steps, +-65 m (a fall past the map's limit respawns at
// -20 m; the level spans +-41 m).
constexpr float kPositionScale = 500.0f;
// Yaw: a turn in 65536 steps. Pitch: +-pi/2 in +-16384.
constexpr float kYawScale = 65536.0f / static_cast<float>(Math::TAU);
constexpr float kPitchScale = 16384.0f / static_cast<float>(Math::PI / 2.0);
// Move: +-1 in +-127.
constexpr float kMoveScale = 127.0f;
// View ticks: 1/256ths.
constexpr double kViewTickScale = 256.0;

// Button and state flags.
enum Flags : uint8_t {
  kJump = 1 << 0,
  kSprint = 1 << 1,
  kFire = 1 << 2,
  kReload = 1 << 3,
  kOnFloor = 1 << 4,
  kDead = 1 << 6,
};

// Which groups of a remote player's fields an entry carries.
enum RemoteFields : uint8_t {
  kRemotePosition = 1 << 0,
  kRemoteLook = 1 << 1,
  kRemoteFlags = 1 << 2,
  kRemoteShots = 1 << 3,
  kRemoteRespawns = 1 << 4,
  kRemoteScore = 1 << 5,
};

// ...and of the client's own player's full state.
enum LocalFields : uint8_t {
  kLocalMotion = 1 << 0,
  kLocalLook = 1 << 1,
  kLocalRecoil = 1 << 2,
  kLocalLanding = 1 << 3,
  kLocalCounters = 1 << 4,
  kLocalFlags = 1 << 5,
  kLocalWeapon = 1 << 6,
  kLocalScore = 1 << 7,
};

// A target's dead flag is sent as "toggled", as it has only two values.
enum TargetFields : uint8_t {
  kTargetPosition = 1 << 0,
  kTargetDeadToggled = 1 << 1,
};

// The player list: the baseline's again, or listed.
enum Roster : uint8_t {
  kRosterSame = 0,
  kRosterListed = 1,
};

int16_t QuantizePosition(float value) {
  if (!std::isfinite(value)) {
    return 0;
  }
  return static_cast<int16_t>(
      std::clamp(std::lround(value * kPositionScale), -32767L, 32767L));
}

float PositionFrom(int16_t value) {
  return static_cast<float>(value) / kPositionScale;
}

uint16_t QuantizeYaw(float value) {
  if (!std::isfinite(value)) {
    return 0;
  }
  // Wrapped: any number of turns.
  return static_cast<uint16_t>(
      static_cast<int32_t>(std::lround(value * kYawScale)) & 0xffff);
}

float YawFrom(uint16_t value) {
  // -pi..pi, like FpsCharacter's.
  return static_cast<float>(static_cast<int16_t>(value)) / kYawScale;
}

int16_t QuantizePitch(float value) {
  if (!std::isfinite(value)) {
    return 0;
  }
  return static_cast<int16_t>(
      std::clamp(std::lround(value * kPitchScale), -16384L, 16384L));
}

float PitchFrom(int16_t value) {
  return static_cast<float>(value) / kPitchScale;
}

int8_t QuantizeMove(float value) {
  if (!std::isfinite(value)) {
    return 0;
  }
  return static_cast<int8_t>(
      std::clamp(std::lround(value * kMoveScale), -127L, 127L));
}

float MoveFrom(int8_t value) {
  return static_cast<float>(value) / kMoveScale;
}

Vector3 QuantizedPosition(const Vector3& position) {
  return Vector3(PositionFrom(QuantizePosition(position.x)),
                 PositionFrom(QuantizePosition(position.y)),
                 PositionFrom(QuantizePosition(position.z)));
}

class ByteWriter {
 public:
  explicit ByteWriter(MessageType type) {
    U8(kProtocolVersion);
    U8(static_cast<uint8_t>(type));
  }

  void U8(uint8_t value) { bytes_.push_back(value); }
  void U16(uint16_t value) { Raw(&value, sizeof(value)); }
  void I16(int16_t value) { Raw(&value, sizeof(value)); }
  void U32(uint32_t value) { Raw(&value, sizeof(value)); }
  void Float(float value) { Raw(&value, sizeof(value)); }
  void Vector(const Vector3& value) {
    Float(value.x);
    Float(value.y);
    Float(value.z);
  }
  void QuantizedVector(const Vector3& value) {
    I16(QuantizePosition(value.x));
    I16(QuantizePosition(value.y));
    I16(QuantizePosition(value.z));
  }
  // Reserve a byte to fill in later (a change mask); returns its offset.
  size_t Placeholder() {
    bytes_.push_back(0);
    return bytes_.size() - 1;
  }
  void Set(size_t offset, uint8_t value) { bytes_[offset] = value; }

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

  uint8_t U8() { return Read<uint8_t>(); }
  uint16_t U16() { return Read<uint16_t>(); }
  int16_t I16() { return Read<int16_t>(); }
  uint32_t U32() { return Read<uint32_t>(); }
  float Float() { return Read<float>(); }
  Vector3 Vector() {
    float x = Float();
    float y = Float();
    float z = Float();
    return Vector3(x, y, z);
  }
  Vector3 QuantizedVector() {
    float x = PositionFrom(I16());
    float y = PositionFrom(I16());
    float z = PositionFrom(I16());
    return Vector3(x, y, z);
  }

  // Whether every read so far was in bounds. Done() also checks that
  // nothing is left over.
  bool ok() const { return ok_; }
  bool Done() const { return ok_ && pos_ == size_; }

 private:
  template <typename T>
  T Read() {
    T value{};
    if (!ok_ || sizeof(T) > size_ - pos_) {
      ok_ = false;
      return value;
    }
    std::memcpy(&value, data_ + pos_, sizeof(T));
    pos_ += sizeof(T);
    return value;
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

// A command's view tick relative to the message's base, in 1/256ths.
int16_t ViewOffset(double view_tick, uint32_t base) {
  double offset = (view_tick - static_cast<double>(base)) * kViewTickScale;
  if (!std::isfinite(offset)) {
    return 0;
  }
  return static_cast<int16_t>(std::clamp(std::lround(offset), -32767L, 32767L));
}

void WriteCommand(const PlayerInput& command,
                  uint32_t view_base,
                  ByteWriter* writer) {
  writer->U8(static_cast<uint8_t>(QuantizeMove(command.move.x)));
  writer->U8(static_cast<uint8_t>(QuantizeMove(command.move.y)));
  writer->U16(QuantizeYaw(command.yaw));
  writer->I16(QuantizePitch(command.pitch));
  writer->U8(command.respawn_count);
  writer->I16(ViewOffset(command.view_tick, view_base));
  writer->U8(ButtonFlags(command));
}

PlayerInput ReadCommand(uint32_t view_base, ByteReader* reader) {
  PlayerInput command;
  command.move.x = MoveFrom(static_cast<int8_t>(reader->U8()));
  command.move.y = MoveFrom(static_cast<int8_t>(reader->U8()));
  command.yaw = YawFrom(reader->U16());
  command.pitch = PitchFrom(reader->I16());
  command.respawn_count = reader->U8();
  command.view_tick =
      static_cast<double>(view_base) + reader->I16() / kViewTickScale;
  SetButtons(reader->U8(), &command);
  return command;
}

// Another player's entry: its remote view against the baseline's (all of
// it for a player new since then).
void WriteRemote(const SnapshotPlayer& player,
                 const SnapshotPlayer& base,
                 ByteWriter* writer) {
  const RemoteView& r = player.remote;
  const RemoteView& b = base.remote;
  size_t mask_at = writer->Placeholder();
  uint8_t mask = 0;
  if (r.position != b.position) {
    mask |= kRemotePosition;
    writer->QuantizedVector(r.position);
  }
  if (r.yaw != b.yaw || r.pitch != b.pitch) {
    mask |= kRemoteLook;
    writer->U16(QuantizeYaw(r.yaw));
    writer->I16(QuantizePitch(r.pitch));
  }
  if (r.dead != b.dead || r.on_floor != b.on_floor) {
    mask |= kRemoteFlags;
    writer->U8((r.dead ? kDead : 0) | (r.on_floor ? kOnFloor : 0));
  }
  if (r.shot_count != b.shot_count) {
    mask |= kRemoteShots;
    writer->U8(r.shot_count);
  }
  if (r.respawn_count != b.respawn_count) {
    mask |= kRemoteRespawns;
    writer->U8(r.respawn_count);
  }
  if (player.kills != base.kills || player.deaths != base.deaths) {
    mask |= kRemoteScore;
    writer->U16(player.kills);
    writer->U16(player.deaths);
  }
  writer->Set(mask_at, mask);
}

void ReadRemote(const SnapshotPlayer& base,
                ByteReader* reader,
                SnapshotPlayer* player) {
  RemoteView& r = player->remote;
  r = base.remote;
  player->kills = base.kills;
  player->deaths = base.deaths;
  uint8_t mask = reader->U8();
  if (mask & kRemotePosition) {
    r.position = reader->QuantizedVector();
  }
  if (mask & kRemoteLook) {
    r.yaw = YawFrom(reader->U16());
    r.pitch = PitchFrom(reader->I16());
  }
  if (mask & kRemoteFlags) {
    uint8_t flags = reader->U8();
    r.dead = flags & kDead;
    r.on_floor = flags & kOnFloor;
  }
  if (mask & kRemoteShots) {
    r.shot_count = reader->U8();
  }
  if (mask & kRemoteRespawns) {
    r.respawn_count = reader->U8();
  }
  if (mask & kRemoteScore) {
    player->kills = reader->U16();
    player->deaths = reader->U16();
  }
  player->state = r.ToState();
}

uint8_t StateFlags(const PlayerState& state) {
  return ButtonFlags(state.prev_input) | (state.on_floor ? kOnFloor : 0) |
         (state.dead ? kDead : 0);
}

bool SameWeapon(const WeaponState& a, const WeaponState& b) {
  return a.ammo == b.ammo && a.reserve_ammo == b.reserve_ammo &&
         a.reloading == b.reloading && a.reload_timer == b.reload_timer &&
         a.fire_cooldown == b.fire_cooldown;
}

// The client's own player's entry: its full state against the baseline's,
// exact (prediction compares it with its own).
void WriteLocal(const SnapshotPlayer& player,
                const SnapshotPlayer& base,
                ByteWriter* writer) {
  const PlayerState& s = player.state;
  const PlayerState& b = base.state;
  size_t mask_at = writer->Placeholder();
  uint8_t mask = 0;
  if (s.position != b.position || s.velocity != b.velocity) {
    mask |= kLocalMotion;
    writer->Vector(s.position);
    writer->Vector(s.velocity);
  }
  if (s.yaw != b.yaw || s.pitch != b.pitch) {
    mask |= kLocalLook;
    writer->Float(s.yaw);
    writer->Float(s.pitch);
  }
  if (s.recoil != b.recoil || s.recoil_yaw != b.recoil_yaw) {
    mask |= kLocalRecoil;
    writer->Float(s.recoil);
    writer->Float(s.recoil_yaw);
  }
  if (s.landing_offset != b.landing_offset ||
      s.landing_velocity != b.landing_velocity) {
    mask |= kLocalLanding;
    writer->Float(s.landing_offset);
    writer->Float(s.landing_velocity);
  }
  if (s.shot_count != b.shot_count || s.respawn_count != b.respawn_count) {
    mask |= kLocalCounters;
    writer->U32(s.shot_count);
    writer->U8(s.respawn_count);
  }
  if (StateFlags(s) != StateFlags(b) || s.health != b.health) {
    mask |= kLocalFlags;
    writer->U8(StateFlags(s));
    writer->U8(s.health);
  }
  if (!SameWeapon(s.weapon, b.weapon)) {
    mask |= kLocalWeapon;
    writer->U16(static_cast<uint16_t>(s.weapon.ammo));
    writer->U16(static_cast<uint16_t>(s.weapon.reserve_ammo));
    writer->U8(s.weapon.reloading ? 1 : 0);
    writer->Float(s.weapon.reload_timer);
    writer->Float(s.weapon.fire_cooldown);
  }
  if (player.kills != base.kills || player.deaths != base.deaths) {
    mask |= kLocalScore;
    writer->U16(player.kills);
    writer->U16(player.deaths);
  }
  writer->Set(mask_at, mask);
}

void ReadLocal(const SnapshotPlayer& base,
               ByteReader* reader,
               SnapshotPlayer* player) {
  PlayerState& s = player->state;
  s = base.state;
  player->kills = base.kills;
  player->deaths = base.deaths;
  uint8_t mask = reader->U8();
  if (mask & kLocalMotion) {
    s.position = reader->Vector();
    s.velocity = reader->Vector();
  }
  if (mask & kLocalLook) {
    s.yaw = reader->Float();
    s.pitch = reader->Float();
  }
  if (mask & kLocalRecoil) {
    s.recoil = reader->Float();
    s.recoil_yaw = reader->Float();
  }
  if (mask & kLocalLanding) {
    s.landing_offset = reader->Float();
    s.landing_velocity = reader->Float();
  }
  if (mask & kLocalCounters) {
    s.shot_count = reader->U32();
    s.respawn_count = reader->U8();
  }
  if (mask & kLocalFlags) {
    uint8_t flags = reader->U8();
    SetButtons(flags, &s.prev_input);
    s.on_floor = flags & kOnFloor;
    s.dead = flags & kDead;
    s.health = reader->U8();
  }
  if (mask & kLocalWeapon) {
    s.weapon.ammo = reader->U16();
    s.weapon.reserve_ammo = reader->U16();
    s.weapon.reloading = reader->U8() != 0;
    s.weapon.reload_timer = reader->Float();
    s.weapon.fire_cooldown = reader->Float();
  }
  if (mask & kLocalScore) {
    player->kills = reader->U16();
    player->deaths = reader->U16();
  }
  player->remote = MakeRemoteView(s);
}

const SnapshotPlayer* FindPlayer(const SnapshotMessage* snapshot,
                                 int32_t peer_id) {
  if (!snapshot) {
    return nullptr;
  }
  for (const SnapshotPlayer& player : snapshot->players) {
    if (player.peer_id == peer_id) {
      return &player;
    }
  }
  return nullptr;
}

bool SameRoster(const SnapshotMessage& a, const SnapshotMessage& b) {
  if (a.players.size() != b.players.size()) {
    return false;
  }
  for (size_t i = 0; i < a.players.size(); i++) {
    if (a.players[i].peer_id != b.players[i].peer_id) {
      return false;
    }
  }
  return true;
}

}  // namespace

PlayerInput QuantizeCommand(const PlayerInput& command) {
  PlayerInput quantized = command;
  quantized.move = Vector2(MoveFrom(QuantizeMove(command.move.x)),
                           MoveFrom(QuantizeMove(command.move.y)));
  quantized.yaw = YawFrom(QuantizeYaw(command.yaw));
  quantized.pitch = PitchFrom(QuantizePitch(command.pitch));
  // On the wire the view tick is relative to a whole tick, so 1/256ths of
  // a tick whatever the base.
  quantized.view_tick =
      std::isfinite(command.view_tick)
          ? std::round(command.view_tick * kViewTickScale) / kViewTickScale
          : 0.0;
  return quantized;
}

PlayerState RemoteView::ToState() const {
  PlayerState state;
  state.position = position;
  state.yaw = yaw;
  state.pitch = pitch;
  state.dead = dead;
  state.on_floor = on_floor;
  state.shot_count = shot_count;
  state.respawn_count = respawn_count;
  return state;
}

RemoteView MakeRemoteView(const PlayerState& state) {
  RemoteView view;
  view.position = QuantizedPosition(state.position);
  view.yaw = YawFrom(QuantizeYaw(state.yaw));
  view.pitch = PitchFrom(QuantizePitch(state.pitch));
  view.dead = state.dead;
  view.on_floor = state.on_floor;
  view.shot_count = static_cast<uint8_t>(state.shot_count);
  view.respawn_count = state.respawn_count;
  return view;
}

void QuantizeSnapshot(SnapshotMessage* snapshot) {
  for (SnapshotPlayer& player : snapshot->players) {
    player.remote = MakeRemoteView(player.state);
  }
  for (SnapshotTarget& target : snapshot->targets) {
    target.position = QuantizedPosition(target.position);
  }
}

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
  for (const PlayerInput& command : message.commands) {
    WriteCommand(command, view_base, &writer);
  }
  return writer.Finish();
}

bool DecodeInput(const PackedByteArray& bytes, InputMessage* message) {
  ByteReader reader(bytes, MessageType::kInput);
  message->sequence = reader.U32();
  message->snapshot_ack = reader.U32();
  int count = reader.U8();
  uint32_t view_base = reader.U32();
  if (!reader.ok() || count < 1 || count > kMaxInputCommands) {
    return false;
  }
  message->commands.clear();
  for (int i = 0; i < count; i++) {
    message->commands.push_back(ReadCommand(view_base, &reader));
  }
  return reader.Done();
}

PackedByteArray EncodeSnapshot(const SnapshotMessage& message,
                               const SnapshotMessage* baseline) {
  ByteWriter writer(MessageType::kSnapshot);
  writer.U32(message.tick);
  writer.U32(baseline ? baseline->tick : 0);
  writer.U32(static_cast<uint32_t>(message.local_peer_id));
  writer.U32(message.ack);
  writer.U8(static_cast<uint8_t>(message.input_buffer));

  if (baseline && SameRoster(message, *baseline)) {
    writer.U8(kRosterSame);
  } else {
    writer.U8(kRosterListed);
    writer.U8(static_cast<uint8_t>(message.players.size()));
    for (const SnapshotPlayer& player : message.players) {
      writer.U32(static_cast<uint32_t>(player.peer_id));
    }
  }
  const SnapshotPlayer empty;
  for (const SnapshotPlayer& player : message.players) {
    const SnapshotPlayer* base = FindPlayer(baseline, player.peer_id);
    if (player.peer_id == message.local_peer_id) {
      WriteLocal(player, base ? *base : empty, &writer);
    } else {
      WriteRemote(player, base ? *base : empty, &writer);
    }
  }

  writer.U16(static_cast<uint16_t>(message.targets.size()));
  const SnapshotTarget empty_target;
  for (size_t i = 0; i < message.targets.size(); i++) {
    const SnapshotTarget& target = message.targets[i];
    const SnapshotTarget& base = baseline && i < baseline->targets.size()
                                     ? baseline->targets[i]
                                     : empty_target;
    size_t mask_at = writer.Placeholder();
    uint8_t mask = 0;
    if (target.position != base.position) {
      mask |= kTargetPosition;
      writer.QuantizedVector(target.position);
    }
    if (target.dead != base.dead) {
      mask |= kTargetDeadToggled;
    }
    writer.Set(mask_at, mask);
  }
  return writer.Finish();
}

bool PeekSnapshotBaseline(const PackedByteArray& bytes, uint32_t* baseline) {
  ByteReader reader(bytes, MessageType::kSnapshot);
  reader.U32();
  *baseline = reader.U32();
  return reader.ok();
}

bool DecodeSnapshot(const PackedByteArray& bytes,
                    const SnapshotMessage* baseline,
                    SnapshotMessage* message) {
  ByteReader reader(bytes, MessageType::kSnapshot);
  message->tick = reader.U32();
  message->baseline = reader.U32();
  message->local_peer_id = static_cast<int32_t>(reader.U32());
  message->ack = reader.U32();
  message->input_buffer = static_cast<int8_t>(reader.U8());
  if (!reader.ok() || (message->baseline != 0) != (baseline != nullptr) ||
      (baseline && baseline->tick != message->baseline)) {
    return false;
  }

  std::vector<int32_t> peer_ids;
  uint8_t roster = reader.U8();
  if (roster == kRosterSame && baseline) {
    for (const SnapshotPlayer& player : baseline->players) {
      peer_ids.push_back(player.peer_id);
    }
  } else if (roster == kRosterListed) {
    int count = reader.U8();
    if (!reader.ok() || count > kMaxSnapshotPlayers) {
      return false;
    }
    for (int i = 0; i < count; i++) {
      peer_ids.push_back(static_cast<int32_t>(reader.U32()));
    }
  } else {
    return false;
  }
  message->players.clear();
  const SnapshotPlayer empty;
  for (int32_t peer_id : peer_ids) {
    if (!reader.ok()) {
      return false;
    }
    const SnapshotPlayer* base = FindPlayer(baseline, peer_id);
    SnapshotPlayer player;
    player.peer_id = peer_id;
    if (peer_id == message->local_peer_id) {
      ReadLocal(base ? *base : empty, &reader, &player);
    } else {
      ReadRemote(base ? *base : empty, &reader, &player);
    }
    message->players.push_back(player);
  }

  int target_count = reader.U16();
  if (!reader.ok() || target_count > kMaxSnapshotTargets) {
    return false;
  }
  message->targets.clear();
  const SnapshotTarget empty_target;
  for (int i = 0; i < target_count && reader.ok(); i++) {
    const SnapshotTarget& base =
        baseline && static_cast<size_t>(i) < baseline->targets.size()
            ? baseline->targets[i]
            : empty_target;
    SnapshotTarget target = base;
    uint8_t mask = reader.U8();
    if (mask & kTargetPosition) {
      target.position = reader.QuantizedVector();
    }
    if (mask & kTargetDeadToggled) {
      target.dead = !base.dead;
    }
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
  if (type < static_cast<uint8_t>(EventType::kHit) ||
      type > static_cast<uint8_t>(EventType::kDamaged)) {
    return false;
  }
  message->type = static_cast<EventType>(type);
  return reader.Done();
}

}  // namespace godot
