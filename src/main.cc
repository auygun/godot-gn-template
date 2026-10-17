#include "src/main.h"

#include <algorithm>
#include <set>
#include <utility>

#include <godot_cpp/classes/e_net_connection.hpp>
#include <godot_cpp/classes/e_net_multiplayer_peer.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/multiplayer_peer.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "src/fps_character.h"
#include "src/hud.h"
#include "src/target.h"
#include "src/weapon.h"

namespace godot {

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

Main::Main() = default;

Main::~Main() = default;

void Main::_ready() {
  // Before the players' own _physics_process (which place the camera and
  // aim the weapon from the state this tick produces).
  set_physics_process_priority(-1);

  hud_ = Object::cast_to<Hud>(get_node_or_null("Hud"));
  players_root_ = get_node_or_null("Players");
  if (!players_root_) {
    players_root_ = this;
  }

  Node* targets = get_node_or_null("Targets");
  if (targets) {
    for (int i = 0; i < targets->get_child_count(); i++) {
      Target* target = Object::cast_to<Target>(targets->get_child(i));
      if (target) {
        targets_.push_back(target);
      }
    }
  }
  Node* spawn_points = get_node_or_null("Level/SpawnPoints");
  if (spawn_points) {
    for (int i = 0; i < spawn_points->get_child_count(); i++) {
      Node3D* point = Object::cast_to<Node3D>(spawn_points->get_child(i));
      if (point) {
        spawn_points_.push_back(point);
      }
    }
  }

  if (!ParseCommandLine()) {
    UtilityFunctions::push_error(
        "Usage: -- [--host [port] | --server [port] | --join <address> "
        "[port]] [--lag <ms>] [--loss <percent>] [--netstats]");
    get_tree()->quit(1);
    return;
  }
  if (!player_scene_.is_valid()) {
    UtilityFunctions::push_error("Main: player_scene is not set");
    return;
  }
  if (!StartNetwork()) {
    get_tree()->quit(1);
    return;
  }

  if (mode_ == Mode::kClient) {
    // The server simulates them.
    for (Target* target : targets_) {
      target->set_simulated(false);
    }
  } else if (mode_ != Mode::kDedicated) {
    local_peer_id_ = kServerPeerId;
    SpawnPlayer(local_peer_id_, NextSpawnTransform());
  }
}

bool Main::ParseCommandLine() {
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

bool Main::StartNetwork() {
  multiplayer_ = get_multiplayer();
  if (mode_ == Mode::kOffline) {
    return true;
  }
  if (!multiplayer_.is_valid()) {
    UtilityFunctions::push_error("Main: no SceneMultiplayer");
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
        vformat("Main: can't %s port %d: %s",
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
  multiplayer_->connect("peer_packet", callable_mp(this, &Main::OnPeerPacket));

  if (mode_ == Mode::kClient) {
    multiplayer_->connect("connected_to_server",
                          callable_mp(this, &Main::OnConnectedToServer));
    multiplayer_->connect("connection_failed",
                          callable_mp(this, &Main::OnConnectionFailed));
    multiplayer_->connect("server_disconnected",
                          callable_mp(this, &Main::OnServerDisconnected));
    UtilityFunctions::print(
        vformat("Main: connecting to %s:%d", address_, port_));
  } else {
    multiplayer_->connect("peer_connected",
                          callable_mp(this, &Main::OnPeerConnected));
    multiplayer_->connect("peer_disconnected",
                          callable_mp(this, &Main::OnPeerDisconnected));
    UtilityFunctions::print(vformat("Main: listening on port %d", port_));
  }
  if (lag_ms_ > 0 || loss_percent_ > 0) {
    UtilityFunctions::print(vformat("Main: simulating %d ms lag, %d%% loss",
                                    lag_ms_, loss_percent_));
  }
  if (mode_ == Mode::kDedicated) {
    // Nothing to draw: don't spin between ticks.
    Engine* engine = Engine::get_singleton();
    engine->set_max_fps(engine->get_physics_ticks_per_second());
  }
  return true;
}

void Main::_exit_tree() {
  if (mode_ == Mode::kClient) {
    UtilityFunctions::print(
        vformat("Main: predicted %d commands, %d corrections",
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

void Main::_physics_process(double delta) {
  if (mode_ == Mode::kClient) {
    ClientTick(static_cast<float>(delta));
  } else {
    ServerTick(static_cast<float>(delta));
  }
  FlushDelayedPackets();
  PrintNetStats(static_cast<float>(delta));
}

void Main::PrintNetStats(float dt) {
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
  UtilityFunctions::print(vformat(
      "Main: netstats up %.1f kbit/s, down %.1f kbit/s, %.0f packets/s up, "
      "%d peers",
      net_stats_sent_ * 8.0 / 1000.0 / seconds,
      net_stats_received_ * 8.0 / 1000.0 / seconds,
      net_stats_packets_ / seconds, static_cast<int>(clients_.size())));
  net_stats_timer_ = 0.0f;
  net_stats_sent_ = 0;
  net_stats_received_ = 0;
  net_stats_packets_ = 0;
}

FpsCharacter* Main::SpawnPlayer(int peer_id, const Transform3D& transform) {
  FpsCharacter* player =
      Object::cast_to<FpsCharacter>(player_scene_->instantiate());
  if (!player) {
    UtilityFunctions::push_error("Main: player_scene isn't an FpsCharacter");
    return nullptr;
  }
  player->set_name(vformat("Player%d", peer_id));
  player->set_local(peer_id == local_peer_id_);
  // Before entering the tree, where it takes this as its spawn.
  player->set_transform(transform);
  players_root_->add_child(player);
  players_[peer_id] = player;

  Weapon* weapon = player->weapon();
  if (weapon && is_server()) {
    weapon->SetShotResolver(this);
    weapon->connect("hit", callable_mp(this, &Main::OnWeaponHit).bind(peer_id));
    weapon->connect("killed",
                    callable_mp(this, &Main::OnWeaponKilled).bind(peer_id));
  }
  if (is_server()) {
    player->connect("damaged",
                    callable_mp(this, &Main::OnPlayerDamaged).bind(peer_id));
    scores_[peer_id] = Score();
  }
  if (peer_id == local_peer_id_) {
    if (weapon) {
      weapon->connect("ammo_changed", callable_mp(this, &Main::OnAmmoChanged));
      OnAmmoChanged(weapon->ammo(), weapon->reserve_ammo(), false);
    }
    UtilityFunctions::print(vformat("Main: spawned local player %d", peer_id));
  }
  return player;
}

void Main::DespawnPlayer(int peer_id) {
  auto it = players_.find(peer_id);
  if (it == players_.end()) {
    return;
  }
  it->second->queue_free();
  players_.erase(it);
  scores_.erase(peer_id);
  respawn_timers_.erase(peer_id);
}

Transform3D Main::NextSpawnTransform() {
  if (spawn_points_.empty()) {
    return Transform3D();
  }
  Node3D* point = spawn_points_[next_spawn_ % spawn_points_.size()];
  next_spawn_++;
  return point->get_global_transform();
}

Transform3D Main::SafestSpawnTransform(const FpsCharacter* exclude) {
  if (spawn_points_.empty()) {
    return NextSpawnTransform();
  }
  // The one whose nearest living player is farthest away.
  Node3D* best = nullptr;
  float best_distance = -1.0f;
  for (Node3D* point : spawn_points_) {
    Vector3 position = point->get_global_position();
    float nearest = 1e9f;
    for (const auto& [peer_id, player] : players_) {
      if (player != exclude && !player->dead()) {
        nearest = std::min(nearest, position.distance_squared_to(
                                        player->get_global_position()));
      }
    }
    if (nearest > best_distance) {
      best_distance = nearest;
      best = point;
    }
  }
  return best->get_global_transform();
}

void Main::ServerTick(float dt) {
  tick_++;
  // Respawn the dead whose wait is over, before this tick's commands.
  for (auto it = respawn_timers_.begin(); it != respawn_timers_.end();) {
    it->second -= dt;
    auto player = players_.find(it->first);
    if (player == players_.end()) {
      it = respawn_timers_.erase(it);
    } else if (it->second <= 0.0f) {
      // Unless something else (a script) respawned it meanwhile.
      if (player->second->dead()) {
        player->second->RespawnAt(SafestSpawnTransform(player->second));
      }
      it = respawn_timers_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& [peer_id, player] : players_) {
    if (peer_id == local_peer_id_) {
      // It sees the bodies as they are (as of the last tick).
      PlayerInput command = player->GatherInput();
      command.view_tick = static_cast<double>(tick_ - 1);
      player->Simulate(command, dt);
      continue;
    }
    auto client = clients_.find(peer_id);
    if (client == clients_.end()) {
      continue;
    }
    ClientConnection& connection = client->second;
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
    // A client sees the bodies in the past, but not further than
    // kMaxRewindTicks, nor in the future.
    PlayerInput command = connection.last_command;
    double now = static_cast<double>(tick_ - 1);
    if (!(command.view_tick >= now - kMaxRewindTicks)) {  // Also NaN.
      command.view_tick = now - kMaxRewindTicks;
    }
    command.view_tick = std::min(command.view_tick, now);
    player->Simulate(command, dt);
  }
  RecordHitboxes();
  if (mode_ != Mode::kOffline && tick_ % kSnapshotInterval == 0) {
    SendSnapshots();
  }
  UpdateHud();
}

void Main::RecordHitboxes() {
  // After the players moved; targets move in their own _physics_process,
  // after this, so theirs are a tick old, which is what clients see too
  // (the snapshot is taken here).
  std::vector<LagCompensation::Body> bodies;
  for (const auto& [peer_id, player] : players_) {
    bodies.push_back({player, !player->dead()});
  }
  for (Target* target : targets_) {
    bodies.push_back({target, !target->dead()});
  }
  lag_compensation_.Record(tick_, bodies);
}

TypedArray<RID> Main::ShootableBodies() {
  TypedArray<RID> bodies;
  for (const auto& [peer_id, player] : players_) {
    bodies.push_back(player->get_rid());
  }
  for (Target* target : targets_) {
    bodies.push_back(target->get_rid());
  }
  return bodies;
}

Node3D* Main::ResolveShot(const Node3D* shooter,
                          double view_tick,
                          const Vector3& origin,
                          const Vector3& direction,
                          float max_distance,
                          Vector3* position,
                          Vector3* normal) {
  Node3D* body = lag_compensation_.Raycast(
      view_tick, origin, direction, max_distance, shooter, position, normal);
  // It may have died since (a body is only shot once dead in the past if
  // it's still alive now).
  FpsCharacter* player = Object::cast_to<FpsCharacter>(body);
  Target* target = Object::cast_to<Target>(body);
  if ((player && player->dead()) || (target && target->dead())) {
    return nullptr;
  }
  return body;
}

void Main::SendSnapshots() {
  if (clients_.empty()) {
    return;
  }
  // The world, as every client will see it (quantized), kept for the
  // deltas against it.
  SnapshotMessage snapshot;
  snapshot.tick = tick_;
  for (const auto& [peer_id, player] : players_) {
    const Score& score = scores_[peer_id];
    SnapshotPlayer entry;
    entry.peer_id = peer_id;
    entry.state = player->CaptureState();
    entry.kills = static_cast<uint16_t>(score.kills);
    entry.deaths = static_cast<uint16_t>(score.deaths);
    snapshot.players.push_back(entry);
  }
  for (Target* target : targets_) {
    snapshot.targets.push_back({target->get_global_position(), target->dead()});
  }
  QuantizeSnapshot(&snapshot);
  KeepSnapshot(snapshot);

  for (const auto& [peer_id, connection] : clients_) {
    snapshot.local_peer_id = peer_id;
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
    const SnapshotMessage* baseline =
        connection.snapshot_ack ? FindSnapshot(connection.snapshot_ack)
                                : nullptr;
    PackedByteArray bytes = EncodeSnapshot(snapshot, baseline);
#ifdef DEBUG_ENABLED
    CheckSnapshotEncoding(bytes, snapshot, baseline);
#endif
    Send(bytes, peer_id, MultiplayerPeer::TRANSFER_MODE_UNRELIABLE);
  }
}

#ifdef DEBUG_ENABLED
void Main::CheckSnapshotEncoding(const PackedByteArray& bytes,
                                 const SnapshotMessage& snapshot,
                                 const SnapshotMessage* baseline) {
  // What the client will decode: the full state of its own player, the
  // quantized view of the rest, exactly. A client's baseline is what it
  // decoded, which matches the server's kept snapshot in the fields it
  // uses (checked here each time).
  SnapshotMessage decoded;
  bool ok = DecodeSnapshot(bytes, baseline, &decoded) &&
            decoded.tick == snapshot.tick &&
            decoded.local_peer_id == snapshot.local_peer_id &&
            decoded.ack == snapshot.ack &&
            decoded.input_buffer == snapshot.input_buffer &&
            decoded.players.size() == snapshot.players.size() &&
            decoded.targets == snapshot.targets;
  for (size_t i = 0; ok && i < snapshot.players.size(); i++) {
    const SnapshotPlayer& want = snapshot.players[i];
    const SnapshotPlayer& got = decoded.players[i];
    ok = got.peer_id == want.peer_id && got.remote == want.remote &&
         got.kills == want.kills && got.deaths == want.deaths;
    if (ok && want.peer_id == snapshot.local_peer_id) {
      const PlayerState& a = got.state;
      const PlayerState& b = want.state;
      ok = a.position == b.position && a.velocity == b.velocity &&
           a.yaw == b.yaw && a.pitch == b.pitch && a.recoil == b.recoil &&
           a.recoil_yaw == b.recoil_yaw &&
           a.landing_offset == b.landing_offset &&
           a.landing_velocity == b.landing_velocity &&
           a.shot_count == b.shot_count && a.respawn_count == b.respawn_count &&
           a.on_floor == b.on_floor && a.health == b.health &&
           a.dead == b.dead && a.prev_input.jump == b.prev_input.jump &&
           a.prev_input.sprint == b.prev_input.sprint &&
           a.prev_input.fire == b.prev_input.fire &&
           a.prev_input.reload == b.prev_input.reload &&
           a.weapon.ammo == b.weapon.ammo &&
           a.weapon.reserve_ammo == b.weapon.reserve_ammo &&
           a.weapon.reloading == b.weapon.reloading &&
           a.weapon.reload_timer == b.weapon.reload_timer &&
           a.weapon.fire_cooldown == b.weapon.fire_cooldown;
    }
  }
  if (!ok) {
    UtilityFunctions::push_error(vformat(
        "Main: snapshot %d for peer %d doesn't decode to what was encoded "
        "(baseline %d)",
        snapshot.tick, snapshot.local_peer_id, baseline ? baseline->tick : 0));
  }
}
#endif

const SnapshotMessage* Main::FindSnapshot(uint32_t tick) const {
  // Snapshots come every kSnapshotInterval ticks.
  const SnapshotMessage& snapshot =
      snapshot_history_[(tick / kSnapshotInterval) % kSnapshotHistory];
  return tick != 0 && snapshot.tick == tick ? &snapshot : nullptr;
}

void Main::KeepSnapshot(const SnapshotMessage& snapshot) {
  snapshot_history_[(snapshot.tick / kSnapshotInterval) % kSnapshotHistory] =
      snapshot;
}

void Main::ClientTick(float dt) {
  // Which of the players is ours.
  if (local_peer_id_ == 0) {
    return;
  }
  if (snapshot_pending_) {
    snapshot_pending_ = false;
    ReconcileLocalPlayer(snapshot_, dt);
  }
  SnapshotMessage state;
  if (interpolator_.Sample(&state)) {
    ApplyRemoteState(state);
  }

  UpdateHud();

  auto local = players_.find(local_peer_id_);
  if (local == players_.end()) {
    return;
  }
  FpsCharacter* player = local->second;
  // Our own player moves now, under the command the server will get.
  PlayerInput command = player->GatherInput();
  command.view_tick = interpolator_.render_tick();
  // As the server will decode it, so both simulate the same.
  command = QuantizeCommand(command);
  if (prediction_.SkipThisTick()) {
    return;
  }
  prediction_.Predict(player, command, dt);
  SendInput();
}

void Main::ReconcileLocalPlayer(const SnapshotMessage& snapshot, float dt) {
  scores_.clear();
  for (const SnapshotPlayer& entry : snapshot.players) {
    scores_[entry.peer_id] = {entry.kills, entry.deaths};
  }
  for (const SnapshotPlayer& entry : snapshot.players) {
    if (entry.peer_id != local_peer_id_) {
      continue;
    }
    auto it = players_.find(local_peer_id_);
    FpsCharacter* player = it != players_.end() ? it->second : nullptr;
    if (!player) {
      player = SpawnPlayer(
          local_peer_id_,
          Transform3D(Basis(Vector3(0.0f, 1.0f, 0.0f), entry.state.yaw),
                      entry.state.position));
      if (!player) {
        return;
      }
      player->ApplyState(entry.state);
    }
    prediction_.Reconcile(player, snapshot.ack, snapshot.input_buffer,
                          entry.state, dt);
    return;
  }
}

void Main::ApplyRemoteState(const SnapshotMessage& state) {
  std::set<int> present = {local_peer_id_};
  for (const SnapshotPlayer& entry : state.players) {
    if (entry.peer_id <= 0 || entry.peer_id == local_peer_id_) {
      continue;
    }
    present.insert(entry.peer_id);
    auto it = players_.find(entry.peer_id);
    FpsCharacter* player = it != players_.end() ? it->second : nullptr;
    if (!player) {
      player = SpawnPlayer(
          entry.peer_id,
          Transform3D(Basis(Vector3(0.0f, 1.0f, 0.0f), entry.state.yaw),
                      entry.state.position));
      if (!player) {
        continue;
      }
    }
    player->ApplyState(entry.state);
  }
  std::vector<int> gone;
  for (const auto& [peer_id, player] : players_) {
    if (!present.count(peer_id)) {
      gone.push_back(peer_id);
    }
  }
  for (int peer_id : gone) {
    DespawnPlayer(peer_id);
  }

  size_t count = std::min(targets_.size(), state.targets.size());
  for (size_t i = 0; i < count; i++) {
    targets_[i]->ApplyState(state.targets[i].position, state.targets[i].dead);
  }
}

void Main::SendInput() {
  InputMessage message = prediction_.MakeInputMessage();
  message.snapshot_ack = has_snapshot_ ? snapshot_.tick : 0;
  Send(EncodeInput(message), kServerPeerId,
       MultiplayerPeer::TRANSFER_MODE_UNRELIABLE);
}

void Main::SendEvent(int peer_id, EventType type) {
  if (!clients_.count(peer_id)) {
    return;
  }
  EventMessage message;
  message.type = type;
  Send(EncodeEvent(message), peer_id, MultiplayerPeer::TRANSFER_MODE_RELIABLE);
}

void Main::Send(const PackedByteArray& bytes,
                int peer_id,
                MultiplayerPeer::TransferMode mode) {
  if (lag_ms_ == 0 && loss_percent_ == 0) {
    multiplayer_->send_bytes(bytes, peer_id, mode);
    return;
  }
  // Loss only drops unreliable packets, as the network's would be resent.
  if (mode != MultiplayerPeer::TRANSFER_MODE_RELIABLE &&
      static_cast<int>(UtilityFunctions::randi() % 100) < loss_percent_) {
    return;
  }
  uint64_t now = Time::get_singleton()->get_ticks_usec();
  delayed_packets_.push_back(
      {now + static_cast<uint64_t>(lag_ms_) * 1000, bytes, peer_id, mode});
  FlushDelayedPackets();
}

void Main::FlushDelayedPackets() {
  uint64_t now = Time::get_singleton()->get_ticks_usec();
  while (!delayed_packets_.empty() &&
         delayed_packets_.front().send_at_usec <= now) {
    const DelayedPacket& packet = delayed_packets_.front();
    // Not to a client that left meanwhile.
    if (packet.peer_id == kServerPeerId || clients_.count(packet.peer_id)) {
      multiplayer_->send_bytes(packet.bytes, packet.peer_id, packet.mode);
    }
    delayed_packets_.pop_front();
  }
}

void Main::OnPeerConnected(int64_t peer_id) {
  UtilityFunctions::print(vformat("Main: peer %d connected", peer_id));
  clients_[static_cast<int>(peer_id)] = ClientConnection();
  FpsCharacter* player =
      SpawnPlayer(static_cast<int>(peer_id), NextSpawnTransform());
  if (player) {
    // Until its first command, the client looks where the spawn faces.
    clients_[static_cast<int>(peer_id)].last_command.yaw =
        player->get_rotation().y;
  }
}

void Main::OnPeerDisconnected(int64_t peer_id) {
  UtilityFunctions::print(vformat("Main: peer %d disconnected", peer_id));
  clients_.erase(static_cast<int>(peer_id));
  DespawnPlayer(static_cast<int>(peer_id));
}

void Main::OnPeerPacket(int64_t peer_id, const PackedByteArray& bytes) {
  if (!bytes.is_empty() && bytes[0] != kProtocolVersion) {
    if (mode_ == Mode::kClient) {
      UtilityFunctions::push_error(
          vformat("Main: the server runs protocol %d, this is %d", bytes[0],
                  kProtocolVersion));
      get_tree()->quit(1);
    } else {
      UtilityFunctions::push_warning(vformat(
          "Main: peer %d runs protocol %d, dropping it", peer_id, bytes[0]));
      multiplayer_->disconnect_peer(static_cast<int>(peer_id));
    }
    return;
  }
  MessageType type;
  if (!PeekMessageType(bytes, &type)) {
    return;
  }
  if (mode_ == Mode::kClient) {
    if (peer_id != kServerPeerId) {
      return;
    }
    if (type == MessageType::kSnapshot) {
      OnSnapshot(bytes);
    } else if (type == MessageType::kEvent) {
      OnEvent(bytes);
    }
  } else if (type == MessageType::kInput) {
    OnInput(static_cast<int>(peer_id), bytes);
  }
}

void Main::OnConnectedToServer() {
  local_peer_id_ = multiplayer_->get_unique_id();
  UtilityFunctions::print(
      vformat("Main: connected as peer %d", local_peer_id_));
}

void Main::OnConnectionFailed() {
  UtilityFunctions::push_error(
      vformat("Main: can't connect to %s:%d", address_, port_));
  get_tree()->quit(1);
}

void Main::OnServerDisconnected() {
  UtilityFunctions::print("Main: the server disconnected");
  get_tree()->quit();
}

void Main::OnInput(int peer_id, const PackedByteArray& bytes) {
  auto client = clients_.find(peer_id);
  InputMessage message;
  if (client == clients_.end() || !DecodeInput(bytes, &message)) {
    return;
  }
  ClientConnection& connection = client->second;
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

void Main::OnSnapshot(const PackedByteArray& bytes) {
  uint32_t baseline_tick = 0;
  if (!PeekSnapshotBaseline(bytes, &baseline_tick)) {
    return;
  }
  // Encoded against a snapshot we no longer have (or never had): drop it;
  // our acks move the server on to one we have.
  const SnapshotMessage* baseline = nullptr;
  if (baseline_tick != 0) {
    baseline = FindSnapshot(baseline_tick);
    if (!baseline) {
      return;
    }
  }
  SnapshotMessage snapshot;
  if (!DecodeSnapshot(bytes, baseline, &snapshot) ||
      snapshot.local_peer_id != local_peer_id_ || snapshot.tick == 0) {
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

void Main::OnEvent(const PackedByteArray& bytes) {
  EventMessage message;
  if (!DecodeEvent(bytes, &message) || !hud_) {
    return;
  }
  if (message.type == EventType::kHit) {
    hud_->ShowHitmarker();
  } else if (message.type == EventType::kDamaged) {
    hud_->ShowDamage();
  }
}

void Main::OnWeaponHit(Object* collider, int64_t peer_id) {
  if (peer_id != local_peer_id_) {
    SendEvent(static_cast<int>(peer_id), EventType::kHit);
  } else if (hud_) {
    hud_->ShowHitmarker();
  }
}

void Main::OnWeaponKilled(Object* victim, int64_t peer_id) {
  // Targets are only for practice.
  FpsCharacter* victim_player = Object::cast_to<FpsCharacter>(victim);
  if (!victim_player) {
    return;
  }
  int victim_id = 0;
  for (const auto& [id, player] : players_) {
    if (player == victim_player) {
      victim_id = id;
    }
  }
  scores_[static_cast<int>(peer_id)].kills++;
  scores_[victim_id].deaths++;
  respawn_timers_[victim_id] = kRespawnDelay;
  UtilityFunctions::print(
      vformat("Main: player %d killed player %d", peer_id, victim_id));
  if (peer_id != local_peer_id_) {
    SendEvent(static_cast<int>(peer_id), EventType::kKill);
  }
}

void Main::OnPlayerDamaged(int64_t peer_id) {
  if (peer_id != local_peer_id_) {
    SendEvent(static_cast<int>(peer_id), EventType::kDamaged);
  } else if (hud_) {
    hud_->ShowDamage();
  }
}

void Main::UpdateHud() {
  auto local = players_.find(local_peer_id_);
  if (!hud_ || local == players_.end()) {
    return;
  }
  const Score& score = scores_[local_peer_id_];
  hud_->UpdateStatus(local->second->health(), local->second->dead(),
                     score.kills, score.deaths);
}

void Main::OnAmmoChanged(int ammo, int reserve, bool reloading) {
  if (hud_) {
    hud_->UpdateAmmo(ammo, reserve, reloading);
  }
}

// static
void Main::_bind_methods() {
  ClassDB::bind_method(D_METHOD("set_player_scene", "scene"),
                       &Main::set_player_scene);
  ClassDB::bind_method(D_METHOD("get_player_scene"), &Main::player_scene);

  ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "player_scene",
                            PROPERTY_HINT_RESOURCE_TYPE, "PackedScene"),
               "set_player_scene", "get_player_scene");
}

}  // namespace godot
