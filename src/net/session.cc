#include "src/net/session.h"

#include <algorithm>
#include <set>
#include <utility>

#include <godot_cpp/classes/e_net_connection.hpp>
#include <godot_cpp/classes/e_net_multiplayer_peer.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot::net {

namespace {

// ENet's packet compression; the same on every side (not negotiated).
constexpr ENetConnection::CompressionMode kCompressionMode =
    ENetConnection::COMPRESS_RANGE_CODER;

// A port argument: 1..65535, or 0 if it isn't one.
int ParsePort(const String& arg) {
  if (!arg.is_valid_int()) {
    return 0;
  }
  int64_t port = arg.to_int();
  return port >= 1 && port <= 65535 ? static_cast<int>(port) : 0;
}

}  // namespace

NetSession::NetSession() = default;

NetSession::~NetSession() = default;

void NetSession::_ready() {
  // Before every other node's: they see this tick's result.
  set_physics_process_priority(-1);
}

bool NetSession::Start() {
  ERR_FAIL_COND_V_MSG(!delegate_, false, "NetSession: no delegate");
  ERR_FAIL_COND_V_MSG(!schema_.command(), false, "NetSession: no command");
  if (!ParseCommandLine()) {
    UtilityFunctions::push_error(
        "Usage: -- [--host [port] | --server [port] | --join <address> "
        "[port]] [--lag <ms>] [--loss <percent>] [--netstats]");
    return false;
  }
  if (!StartNetwork()) {
    return false;
  }
  if (is_server()) {
    local_peer_ = kServerPeerId;
  }
  started_ = true;
  return true;
}

bool NetSession::ParseCommandLine() {
  PackedStringArray args = OS::get_singleton()->get_cmdline_user_args();
  int i = 0;
  if (i < args.size() && (args[i] == "--host" || args[i] == "--server")) {
    mode_ = args[i] == "--host" ? Mode::kHost : Mode::kDedicated;
    i++;
  } else if (i < args.size() && args[i] == "--join") {
    if (i + 1 >= args.size()) {
      return false;
    }
    mode_ = Mode::kClient;
    address_ = args[i + 1];
    i += 2;
  }
  if (mode_ != Mode::kOffline && i < args.size() &&
      !args[i].begins_with("--")) {
    port_ = ParsePort(args[i]);
    if (port_ == 0) {
      return false;
    }
    i++;
  }
  while (i < args.size()) {
    if (args[i] == "--netstats") {
      net_stats_ = true;
      i++;
      continue;
    }
    if (i + 1 >= args.size() || !args[i + 1].is_valid_int()) {
      return false;
    }
    int64_t value = args[i + 1].to_int();
    if (args[i] == "--lag" && value >= 0 && value <= kMaxLagMs) {
      lag_ms_ = static_cast<int>(value);
    } else if (args[i] == "--loss" && value >= 0 && value <= 100) {
      loss_percent_ = static_cast<int>(value);
    } else {
      return false;
    }
    i += 2;
  }
  return true;
}

bool NetSession::StartNetwork() {
  multiplayer_ = get_multiplayer();
  if (mode_ == Mode::kOffline) {
    return true;
  }
  if (!multiplayer_.is_valid()) {
    UtilityFunctions::push_error("NetSession: no SceneMultiplayer");
    return false;
  }

  Ref<ENetMultiplayerPeer> peer;
  peer.instantiate();
  Error err;
  if (mode_ == Mode::kClient) {
    err = peer->create_client(address_, port_);
  } else {
    err = peer->create_server(port_, kMaxClients);
  }
  if (err != OK) {
    UtilityFunctions::push_error(
        vformat("NetSession: can't %s port %d: %s",
                mode_ == Mode::kClient ? "connect to " + address_ + " on"
                                       : String("listen on"),
                port_, UtilityFunctions::error_string(err)));
    return false;
  }
  // Every packet compressed on its own (they're unreliable, so each must
  // decode alone). Both sides must use the same mode.
  peer->get_host()->compress(kCompressionMode);
  multiplayer_->set_multiplayer_peer(peer);
  // Clients only talk to the server.
  multiplayer_->set_server_relay_enabled(false);
  multiplayer_->connect("peer_packet",
                        callable_mp(this, &NetSession::OnPeerPacket));

  if (mode_ == Mode::kClient) {
    multiplayer_->connect("connected_to_server",
                          callable_mp(this, &NetSession::OnConnectedToServer));
    multiplayer_->connect("connection_failed",
                          callable_mp(this, &NetSession::OnConnectionFailed));
    multiplayer_->connect("server_disconnected",
                          callable_mp(this, &NetSession::OnServerDisconnected));
    UtilityFunctions::print(
        vformat("NetSession: connecting to %s:%d", address_, port_));
  } else {
    multiplayer_->connect("peer_connected",
                          callable_mp(this, &NetSession::OnPeerConnected));
    multiplayer_->connect("peer_disconnected",
                          callable_mp(this, &NetSession::OnPeerDisconnected));
    UtilityFunctions::print(vformat("NetSession: listening on port %d", port_));
  }
  if (lag_ms_ > 0 || loss_percent_ > 0) {
    UtilityFunctions::print(vformat(
        "NetSession: simulating %d ms lag, %d%% loss", lag_ms_, loss_percent_));
  }
  if (mode_ == Mode::kDedicated) {
    // Nothing to draw: don't spin between ticks.
    Engine* engine = Engine::get_singleton();
    engine->set_max_fps(engine->get_physics_ticks_per_second());
  }
  return true;
}

void NetSession::_exit_tree() {
  if (mode_ == Mode::kClient) {
    UtilityFunctions::print(
        vformat("NetSession: predicted %d commands, %d corrections",
                prediction_.predicted_count(), prediction_.correction_count()));
  }
  if (multiplayer_.is_valid() && mode_ != Mode::kOffline) {
    // Tells the other side now, rather than letting it time out.
    Ref<MultiplayerPeer> peer = multiplayer_->get_multiplayer_peer();
    if (peer.is_valid()) {
      peer->close();
    }
  }
}

EntityId NetSession::AddEntity(Entity* entity, TypeId type, PeerId owner) {
  ERR_FAIL_COND_V_MSG(!schema_.entity_type(type), 0,
                      "NetSession: unknown entity type");
  ERR_FAIL_COND_V_MSG(started_ && !is_server(), 0,
                      "NetSession: clients get entities from the server");
  EntityId id = next_id_++;
  entities_[id] = {entity, type, owner};
  return id;
}

void NetSession::RemoveEntity(EntityId id) {
  ERR_FAIL_COND_MSG(!is_server(), "NetSession: only the server removes");
  Despawn(id);
}

void NetSession::Despawn(EntityId id) {
  auto it = entities_.find(id);
  if (it == entities_.end()) {
    return;
  }
  Entity* entity = it->second.entity;
  entities_.erase(it);
  if (id == own_entity_) {
    own_entity_ = 0;
  }
  delegate_->DespawnEntity(id, entity);
}

Entity* NetSession::FindEntity(EntityId id) const {
  auto it = entities_.find(id);
  return it != entities_.end() ? it->second.entity : nullptr;
}

PeerId NetSession::OwnerOf(EntityId id) const {
  auto it = entities_.find(id);
  return it != entities_.end() ? it->second.owner : kNoOwner;
}

EntityId NetSession::IdOf(const Entity* entity) const {
  for (const auto& [id, record] : entities_) {
    if (record.entity == entity) {
      return id;
    }
  }
  return 0;
}

std::vector<PeerId> NetSession::peers() const {
  std::vector<PeerId> result;
  for (const auto& [peer, connection] : clients_) {
    if (connection.joined) {
      result.push_back(peer);
    }
  }
  return result;
}

void NetSession::_physics_process(double delta) {
  if (!started_) {
    return;
  }
  float dt = static_cast<float>(delta);
  if (mode_ == Mode::kClient) {
    ClientTick(dt);
  } else {
    ServerTick(dt);
  }
  FlushDelayedPackets();
  PrintNetStats(dt);
}

void NetSession::ServerTick(float dt) {
  tick_++;
  delegate_->BeforeServerTick(dt);
  // A copy: simulating may remove entities (and add them).
  std::vector<std::pair<EntityId, EntityRecord>> owned;
  for (const auto& [id, record] : entities_) {
    if (record.owner != kNoOwner) {
      owned.emplace_back(id, record);
    }
  }
  for (const auto& [id, record] : owned) {
    if (!entities_.count(id)) {
      continue;
    }
    if (record.owner == local_peer_) {
      // The server's own player sees the bodies as they are (as of the
      // last tick).
      Box command;
      record.entity->GatherBox(&command);
      record.entity->SimulateBox(command, {dt, SimulationMode::kAuthoritative,
                                           static_cast<double>(tick_ - 1)});
      continue;
    }
    TimedCommand command = NextCommand(record.owner, record.entity);
    record.entity->SimulateBox(
        command.command,
        {dt, SimulationMode::kAuthoritative, command.view_tick});
  }
  RecordHitboxes();
  if (mode_ != Mode::kOffline && tick_ % kSnapshotInterval == 0) {
    SendSnapshots();
  }
}

TimedCommand NetSession::NextCommand(PeerId peer, Entity* entity) {
  auto client = clients_.find(peer);
  TimedCommand command;
  if (client == clients_.end()) {
    entity->IdleBox(&command.command);
  } else {
    ClientConnection& connection = client->second;
    if (connection.last_command.command.empty()) {
      // Until its first command.
      entity->IdleBox(&connection.last_command.command);
    }
    if (connection.next_sequence != 0) {
      // Without the next command (late or lost), repeat the last in its
      // place: buttons are held states and the look is absolute, so that's
      // the best guess.
      auto next = connection.queue.find(connection.next_sequence);
      if (next != connection.queue.end()) {
        connection.last_command = next->second;
        connection.queue.erase(next);
      }
      connection.last_simulated = connection.next_sequence++;
    }
    command = connection.last_command;
  }
  // A client sees the bodies in the past, but not further than
  // kMaxRewindTicks, nor in the future.
  double now = static_cast<double>(tick_ - 1);
  if (!(command.view_tick >= now - kMaxRewindTicks)) {  // Also NaN.
    command.view_tick = now - kMaxRewindTicks;
  }
  command.view_tick = std::min(command.view_tick, now);
  return command;
}

void NetSession::RecordHitboxes() {
  // After the owned entities moved; the rest move in their own
  // _physics_process, after this, so theirs are a tick old, which is what
  // clients see too (the snapshot is taken here).
  std::vector<LagCompensation::Body> bodies;
  for (const auto& [id, record] : entities_) {
    bool active = true;
    Node3D* body = record.entity->HitboxBody(&active);
    if (body) {
      bodies.push_back({body, active});
    }
  }
  lag_compensation_.Record(tick_, bodies);
}

Node3D* NetSession::Raycast(double view_tick,
                            const Vector3& origin,
                            const Vector3& direction,
                            float max_distance,
                            const Node3D* ignore,
                            Vector3* position,
                            Vector3* normal) const {
  Node3D* body = lag_compensation_.Raycast(static_cast<float>(view_tick),
                                           origin, direction, max_distance,
                                           ignore, position, normal);
  if (!body) {
    return nullptr;
  }
  // It may have died since (a body is only hit in the past if it can still
  // be hit now).
  for (const auto& [id, record] : entities_) {
    bool active = true;
    if (record.entity->HitboxBody(&active) == body) {
      return active ? body : nullptr;
    }
  }
  return body;
}

void NetSession::SendSnapshots() {
  if (clients_.empty()) {
    return;
  }
  // The world, kept for the deltas against it.
  Snapshot snapshot;
  snapshot.tick = tick_;
  for (const auto& [id, record] : entities_) {
    SnapshotEntity entry;
    entry.id = id;
    entry.type = record.type;
    entry.owner = record.owner;
    record.entity->CaptureBox(&entry.state);
    snapshot.entities.push_back(std::move(entry));
  }
  KeepSnapshot(snapshot);

  for (const auto& [peer, connection] : clients_) {
    if (!connection.joined) {
      continue;
    }
    snapshot.ack = connection.last_simulated;
    // How far the client's commands arrive ahead of when they're
    // simulated.
    int32_t buffer = connection.last_simulated == 0
                         ? 0
                         : static_cast<int32_t>(connection.last_received -
                                                connection.last_simulated);
    snapshot.input_buffer = static_cast<int8_t>(std::clamp(buffer, -127, 127));
    // Against the newest snapshot the client has; all of it if that's no
    // longer kept.
    const Snapshot* baseline = connection.snapshot_ack
                                   ? FindSnapshot(connection.snapshot_ack)
                                   : nullptr;
    PackedByteArray bytes = EncodeSnapshot(snapshot, baseline, peer);
#ifdef DEBUG_ENABLED
    CheckSnapshotEncoding(bytes, snapshot, baseline, peer);
#endif
    Send(bytes, peer, MultiplayerPeer::TRANSFER_MODE_UNRELIABLE);
  }
}

#ifdef DEBUG_ENABLED
void NetSession::CheckSnapshotEncoding(const PackedByteArray& bytes,
                                       const Snapshot& snapshot,
                                       const Snapshot* baseline,
                                       PeerId peer) {
  // What the client will decode: its own entities' full states, the
  // quantized view of the rest. A client's baseline is what it decoded,
  // which matches the server's kept snapshot in the fields it gets
  // (checked here each time).
  Snapshot decoded;
  bool ok = DecodeSnapshot(bytes, baseline, schema_, peer, &decoded) &&
            decoded.tick == snapshot.tick && decoded.ack == snapshot.ack &&
            decoded.input_buffer == snapshot.input_buffer &&
            decoded.entities.size() == snapshot.entities.size();
  for (size_t i = 0; ok && i < snapshot.entities.size(); i++) {
    const SnapshotEntity& want = snapshot.entities[i];
    const SnapshotEntity& got = decoded.entities[i];
    View view = want.owner == peer ? View::kOwner : View::kOthers;
    ok = got.id == want.id && got.type == want.type &&
         got.owner == want.owner && SameView(got.state, want.state, view);
  }
  if (!ok) {
    UtilityFunctions::push_error(vformat(
        "NetSession: snapshot %d for peer %d doesn't decode to what was "
        "encoded (baseline %d)",
        snapshot.tick, peer, baseline ? baseline->tick : 0));
  }
}
#endif

const Snapshot* NetSession::FindSnapshot(uint32_t tick) const {
  // Snapshots come every kSnapshotInterval ticks.
  const Snapshot& snapshot =
      snapshot_history_[(tick / kSnapshotInterval) % kSnapshotHistory];
  return tick != 0 && snapshot.tick == tick ? &snapshot : nullptr;
}

void NetSession::KeepSnapshot(const Snapshot& snapshot) {
  snapshot_history_[(snapshot.tick / kSnapshotInterval) % kSnapshotHistory] =
      snapshot;
}

void NetSession::ClientTick(float dt) {
  if (local_peer_ == 0) {
    return;
  }
  if (snapshot_pending_) {
    snapshot_pending_ = false;
    ReconcileOwned(snapshot_, dt);
  }
  Snapshot state;
  if (interpolator_.Sample(&state)) {
    ApplyRemote(state);
  }

  Entity* own = FindEntity(own_entity_);
  if (!own) {
    return;
  }
  // Our own entity moves now, under the command the server will get.
  TimedCommand command;
  own->GatherBox(&command.command);
  command.view_tick = interpolator_.render_tick();
  // As the server will decode it, so both simulate the same.
  QuantizeCommand(&command);
  if (prediction_.SkipThisTick()) {
    return;
  }
  prediction_.Predict(own, command, dt);
  SendInput();
}

Entity* NetSession::SpawnFromSnapshot(const SnapshotEntity& entry) {
  Entity* entity =
      delegate_->SpawnEntity(entry.id, entry.type, entry.owner, entry.state);
  if (!entity) {
    return nullptr;
  }
  entities_[entry.id] = {entity, entry.type, entry.owner};
  entity->ApplyBox(entry.state);
  return entity;
}

void NetSession::ReconcileOwned(const Snapshot& snapshot, float dt) {
  // A client predicts the one entity it commands (its player).
  const SnapshotEntity* own = nullptr;
  for (const SnapshotEntity& entry : snapshot.entities) {
    if (entry.owner == local_peer_) {
      own = &entry;
      break;
    }
  }
  if (!own) {
    if (own_entity_ != 0) {
      Despawn(own_entity_);
    }
    return;
  }
  Entity* entity = FindEntity(own->id);
  if (!entity) {
    entity = SpawnFromSnapshot(*own);
    if (!entity) {
      return;
    }
  }
  own_entity_ = own->id;
  prediction_.Reconcile(entity, snapshot.ack, snapshot.input_buffer, own->state,
                        dt);
}

void NetSession::ApplyRemote(const Snapshot& snapshot) {
  std::set<EntityId> present;
  for (const SnapshotEntity& entry : snapshot.entities) {
    present.insert(entry.id);
    if (entry.owner == local_peer_) {
      continue;
    }
    auto it = entities_.find(entry.id);
    if (it == entities_.end()) {
      SpawnFromSnapshot(entry);
      continue;
    }
    if (it->second.type != entry.type || it->second.owner != entry.owner) {
      // A different entity under the id: not one we can draw.
      continue;
    }
    it->second.entity->ApplyBox(entry.state);
  }
  // Gone from the server (but not the scene's, nor our own, which goes by
  // the newest snapshot).
  std::vector<EntityId> gone;
  for (const auto& [id, record] : entities_) {
    if (!present.count(id) && id != own_entity_ &&
        record.owner != local_peer_ && id >= next_id_) {
      gone.push_back(id);
    }
  }
  for (EntityId id : gone) {
    Despawn(id);
  }
}

void NetSession::SendInput() {
  InputMessage message = prediction_.MakeInputMessage();
  message.snapshot_ack = has_snapshot_ ? snapshot_.tick : 0;
  Send(EncodeInput(message), kServerPeerId,
       MultiplayerPeer::TRANSFER_MODE_UNRELIABLE);
}

void NetSession::SendRpc(PeerId peer, const Box& payload) {
  int rpc = schema_.FindRpc(payload.layout()->key);
  ERR_FAIL_COND_MSG(rpc < 0, "NetSession: an RPC not in the schema");
  if (peer == local_peer_) {
    schema_.HandleRpc(rpc, local_peer_, payload);
    return;
  }
  if (is_server()) {
    auto client = clients_.find(peer);
    if (client == clients_.end() || !client->second.joined) {
      return;
    }
  } else if (peer != kServerPeerId) {
    return;
  }
  Send(EncodeRpc(rpc, payload), peer, MultiplayerPeer::TRANSFER_MODE_RELIABLE);
}

void NetSession::Send(const PackedByteArray& bytes,
                      PeerId peer,
                      MultiplayerPeer::TransferMode mode) {
  if (lag_ms_ == 0 && loss_percent_ == 0) {
    multiplayer_->send_bytes(bytes, peer, mode);
    return;
  }
  // Loss only drops unreliable packets, as the network's would be resent.
  if (mode != MultiplayerPeer::TRANSFER_MODE_RELIABLE &&
      static_cast<int>(UtilityFunctions::randi() % 100) < loss_percent_) {
    return;
  }
  uint64_t now = Time::get_singleton()->get_ticks_usec();
  delayed_packets_.push_back(
      {now + static_cast<uint64_t>(lag_ms_) * 1000, bytes, peer, mode});
  FlushDelayedPackets();
}

void NetSession::FlushDelayedPackets() {
  uint64_t now = Time::get_singleton()->get_ticks_usec();
  while (!delayed_packets_.empty() &&
         delayed_packets_.front().send_at_usec <= now) {
    const DelayedPacket& packet = delayed_packets_.front();
    // Not to a client that left meanwhile.
    if (packet.peer == kServerPeerId || clients_.count(packet.peer)) {
      multiplayer_->send_bytes(packet.bytes, packet.peer, packet.mode);
    }
    delayed_packets_.pop_front();
  }
}

void NetSession::PrintNetStats(float dt) {
  if (!net_stats_ || !multiplayer_.is_valid()) {
    return;
  }
  Ref<ENetMultiplayerPeer> peer = multiplayer_->get_multiplayer_peer();
  if (peer.is_null() || peer->get_host().is_null()) {
    return;
  }
  Ref<ENetConnection> host = peer->get_host();
  net_stats_sent_ += static_cast<uint64_t>(
      host->pop_statistic(ENetConnection::HOST_TOTAL_SENT_DATA));
  net_stats_received_ += static_cast<uint64_t>(
      host->pop_statistic(ENetConnection::HOST_TOTAL_RECEIVED_DATA));
  net_stats_packets_ += static_cast<uint64_t>(
      host->pop_statistic(ENetConnection::HOST_TOTAL_SENT_PACKETS));
  net_stats_timer_ += dt;
  if (net_stats_timer_ < kNetStatsInterval) {
    return;
  }
  // kbit/s; packets include ENet's acks and pings.
  double seconds = net_stats_timer_;
  UtilityFunctions::print(
      vformat("NetSession: netstats up %.1f kbit/s, down %.1f kbit/s, %.0f "
              "packets/s up, %d peers",
              net_stats_sent_ * 8.0 / 1000.0 / seconds,
              net_stats_received_ * 8.0 / 1000.0 / seconds,
              net_stats_packets_ / seconds, static_cast<int>(peers().size())));
  net_stats_timer_ = 0.0f;
  net_stats_sent_ = 0;
  net_stats_received_ = 0;
  net_stats_packets_ = 0;
}

void NetSession::OnPeerConnected(int64_t peer) {
  UtilityFunctions::print(vformat("NetSession: peer %d connected", peer));
  // Nothing more until its kHello.
  clients_[static_cast<PeerId>(peer)] = ClientConnection();
}

void NetSession::OnPeerDisconnected(int64_t peer_id) {
  PeerId peer = static_cast<PeerId>(peer_id);
  UtilityFunctions::print(vformat("NetSession: peer %d disconnected", peer));
  auto client = clients_.find(peer);
  if (client == clients_.end()) {
    return;
  }
  bool joined = client->second.joined;
  clients_.erase(client);
  if (!joined) {
    return;
  }
  delegate_->OnPeerLeft(peer);
  std::vector<EntityId> owned;
  for (const auto& [id, record] : entities_) {
    if (record.owner == peer) {
      owned.push_back(id);
    }
  }
  for (EntityId id : owned) {
    Despawn(id);
  }
}

void NetSession::OnPeerPacket(int64_t peer_id, const PackedByteArray& bytes) {
  PeerId peer = static_cast<PeerId>(peer_id);
  if (!bytes.is_empty() && bytes[0] != kWireVersion) {
    if (mode_ == Mode::kClient) {
      UtilityFunctions::push_error(
          vformat("NetSession: the server runs wire version %d, this is %d",
                  bytes[0], kWireVersion));
      get_tree()->quit(1);
    } else {
      UtilityFunctions::push_warning(
          vformat("NetSession: peer %d runs wire version %d, dropping it", peer,
                  bytes[0]));
      multiplayer_->disconnect_peer(peer);
    }
    return;
  }
  MessageType type;
  if (!PeekMessageType(bytes, &type)) {
    return;
  }
  if (mode_ == Mode::kClient) {
    if (peer != kServerPeerId) {
      return;
    }
    if (type == MessageType::kSnapshot) {
      OnSnapshot(bytes);
    } else if (type == MessageType::kRpc) {
      OnRpc(peer, bytes);
    } else if (type == MessageType::kReject) {
      uint32_t hash = 0;
      DecodeHello(bytes, &hash);
      UtilityFunctions::push_error(vformat(
          "NetSession: the server's game differs (schema %x, this is %x)", hash,
          schema_.hash()));
      get_tree()->quit(1);
    }
    return;
  }
  if (type == MessageType::kHello) {
    OnHello(peer, bytes);
    return;
  }
  auto client = clients_.find(peer);
  if (client == clients_.end() || !client->second.joined) {
    return;
  }
  if (type == MessageType::kInput) {
    OnInput(peer, bytes);
  } else if (type == MessageType::kRpc) {
    OnRpc(peer, bytes);
  }
}

void NetSession::OnConnectedToServer() {
  local_peer_ = multiplayer_->get_unique_id();
  UtilityFunctions::print(
      vformat("NetSession: connected as peer %d", local_peer_));
  Send(EncodeHello(MessageType::kHello, schema_.hash()), kServerPeerId,
       MultiplayerPeer::TRANSFER_MODE_RELIABLE);
}

void NetSession::OnConnectionFailed() {
  UtilityFunctions::push_error(
      vformat("NetSession: can't connect to %s:%d", address_, port_));
  get_tree()->quit(1);
}

void NetSession::OnServerDisconnected() {
  UtilityFunctions::print("NetSession: the server disconnected");
  get_tree()->quit();
}

void NetSession::OnHello(PeerId peer, const PackedByteArray& bytes) {
  auto client = clients_.find(peer);
  uint32_t hash = 0;
  if (client == clients_.end() || client->second.joined ||
      !DecodeHello(bytes, &hash)) {
    return;
  }
  if (hash != schema_.hash()) {
    UtilityFunctions::push_warning(vformat(
        "NetSession: peer %d runs a different game (schema %x, this is %x)",
        peer, hash, schema_.hash()));
    // It quits when it gets this.
    Send(EncodeHello(MessageType::kReject, schema_.hash()), peer,
         MultiplayerPeer::TRANSFER_MODE_RELIABLE);
    return;
  }
  client->second.joined = true;
  delegate_->OnPeerJoined(peer);
}

void NetSession::OnInput(PeerId peer, const PackedByteArray& bytes) {
  InputMessage message;
  if (!DecodeInput(bytes, schema_, &message)) {
    return;
  }
  ClientConnection& connection = clients_[peer];
  uint32_t count = static_cast<uint32_t>(message.commands.size());
  if (connection.next_sequence == 0) {
    // The first command: start there.
    connection.next_sequence = message.sequence - (count - 1);
    if (connection.next_sequence == 0) {
      connection.next_sequence = 1;
    }
  }
  if (SequenceNewer(message.sequence, connection.last_received)) {
    connection.last_received = message.sequence;
  }
  // Only forward: an input packet arriving late mustn't make the server
  // encode against a snapshot older than one the client has dropped.
  if (message.snapshot_ack != 0 &&
      (connection.snapshot_ack == 0 ||
       SequenceNewer(message.snapshot_ack, connection.snapshot_ack)) &&
      !SequenceNewer(message.snapshot_ack, tick_)) {
    connection.snapshot_ack = message.snapshot_ack;
  }
  for (uint32_t i = 0; i < count; i++) {
    uint32_t sequence = message.sequence - (count - 1 - i);
    // Not ones simulated already (or repeated in their place), nor far
    // ahead (the client shortens its lead when told about it).
    if (!SequenceNewer(connection.next_sequence, sequence) &&
        sequence - connection.next_sequence < kMaxQueuedCommands) {
      connection.queue.emplace(sequence, message.commands[i]);
    }
  }
}

void NetSession::OnSnapshot(const PackedByteArray& bytes) {
  uint32_t baseline_tick = 0;
  if (!PeekSnapshotBaseline(bytes, &baseline_tick)) {
    return;
  }
  // Encoded against a snapshot we no longer have (or never had): drop it;
  // our acks move the server on to one we have.
  const Snapshot* baseline = nullptr;
  if (baseline_tick != 0) {
    baseline = FindSnapshot(baseline_tick);
    if (!baseline) {
      return;
    }
  }
  Snapshot snapshot;
  if (!DecodeSnapshot(bytes, baseline, schema_, local_peer_, &snapshot) ||
      snapshot.tick == 0) {
    return;
  }
  KeepSnapshot(snapshot);
  // Unreliable: an older one may arrive after a newer (still in time to
  // be drawn, but not to correct the prediction).
  interpolator_.Add(snapshot);
  if (has_snapshot_ && !SequenceNewer(snapshot.tick, snapshot_.tick)) {
    return;
  }
  snapshot_ = std::move(snapshot);
  has_snapshot_ = true;
  snapshot_pending_ = true;
}

void NetSession::OnRpc(PeerId peer, const PackedByteArray& bytes) {
  int rpc = 0;
  Box payload;
  if (DecodeRpc(bytes, schema_, &rpc, &payload)) {
    schema_.HandleRpc(rpc, peer, payload);
  }
}

// static
void NetSession::_bind_methods() {}

}  // namespace godot::net
