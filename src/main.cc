#include "src/main.h"

#include <algorithm>
#include <set>
#include <utility>

#include <godot_cpp/classes/e_net_multiplayer_peer.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/multiplayer_peer.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "src/fps_character.h"
#include "src/hud.h"
#include "src/target.h"
#include "src/weapon.h"

namespace godot {

namespace {

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
        "[port]]");
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
  if (args.is_empty()) {
    return true;
  }
  int port_index = 1;
  if (args[0] == "--host") {
    mode_ = Mode::kHost;
  } else if (args[0] == "--server") {
    mode_ = Mode::kDedicated;
  } else if (args[0] == "--join" && args.size() >= 2) {
    mode_ = Mode::kClient;
    address_ = args[1];
    port_index = 2;
  } else {
    return false;
  }
  if (args.size() > port_index + 1) {
    return false;
  }
  if (args.size() == port_index + 1) {
    port_ = ParsePort(args[port_index]);
  }
  return port_ != 0;
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
  if (mode_ == Mode::kDedicated) {
    // Nothing to draw: don't spin between ticks.
    Engine* engine = Engine::get_singleton();
    engine->set_max_fps(engine->get_physics_ticks_per_second());
  }
  return true;
}

void Main::_exit_tree() {
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
    ClientTick();
  } else {
    ServerTick(static_cast<float>(delta));
  }
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
    weapon->connect("hit", callable_mp(this, &Main::OnWeaponHit).bind(peer_id));
    weapon->connect("killed",
                    callable_mp(this, &Main::OnWeaponKilled).bind(peer_id));
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
}

Transform3D Main::NextSpawnTransform() {
  if (spawn_points_.empty()) {
    return Transform3D();
  }
  Node3D* point = spawn_points_[next_spawn_ % spawn_points_.size()];
  next_spawn_++;
  return point->get_global_transform();
}

void Main::ServerTick(float dt) {
  tick_++;
  for (auto& [peer_id, player] : players_) {
    if (peer_id == local_peer_id_) {
      player->Simulate(player->GatherInput(), dt);
      continue;
    }
    auto client = clients_.find(peer_id);
    if (client == clients_.end()) {
      continue;
    }
    ClientConnection& connection = client->second;
    // Without a new command (late or lost), repeat the last: buttons are
    // held states and the look is absolute, so that's the best guess.
    if (!connection.queue.empty()) {
      connection.last_command = connection.queue.front().command;
      connection.last_simulated = connection.queue.front().sequence;
      connection.queue.pop_front();
    }
    player->Simulate(connection.last_command, dt);
  }
  if (mode_ != Mode::kOffline) {
    SendSnapshots();
  }
}

void Main::SendSnapshots() {
  if (clients_.empty()) {
    return;
  }
  SnapshotMessage snapshot;
  snapshot.tick = tick_;
  for (const auto& [peer_id, player] : players_) {
    snapshot.players.push_back({peer_id, player->CaptureState()});
  }
  for (Target* target : targets_) {
    snapshot.targets.push_back({target->get_global_position(), target->dead()});
  }
  for (const auto& [peer_id, connection] : clients_) {
    snapshot.ack = connection.last_simulated;
    multiplayer_->send_bytes(EncodeSnapshot(snapshot), peer_id,
                             MultiplayerPeer::TRANSFER_MODE_UNRELIABLE);
  }
}

void Main::ClientTick() {
  if (snapshot_pending_) {
    snapshot_pending_ = false;
    ApplySnapshot(snapshot_);
  }
  auto local = players_.find(local_peer_id_);
  if (local != players_.end()) {
    SendInput(local->second->GatherInput());
  }
}

void Main::ApplySnapshot(const SnapshotMessage& snapshot) {
  // Which of the players is ours.
  if (local_peer_id_ == 0) {
    return;
  }
  std::set<int> present;
  for (const SnapshotPlayer& entry : snapshot.players) {
    if (entry.peer_id <= 0) {
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

  size_t count = std::min(targets_.size(), snapshot.targets.size());
  for (size_t i = 0; i < count; i++) {
    targets_[i]->ApplyState(snapshot.targets[i].position,
                            snapshot.targets[i].dead);
  }
}

void Main::SendInput(const PlayerInput& command) {
  sequence_++;
  recent_commands_[sequence_ % kMaxInputCommands] = command;
  InputMessage message;
  message.sequence = sequence_;
  uint32_t count = std::min<uint32_t>(sequence_, kMaxInputCommands);
  for (uint32_t s = sequence_ - count + 1; s != sequence_ + 1; s++) {
    message.commands.push_back(recent_commands_[s % kMaxInputCommands]);
  }
  multiplayer_->send_bytes(EncodeInput(message), kServerPeerId,
                           MultiplayerPeer::TRANSFER_MODE_UNRELIABLE);
}

void Main::SendEvent(int peer_id, EventType type) {
  if (!clients_.count(peer_id)) {
    return;
  }
  EventMessage message;
  message.type = type;
  multiplayer_->send_bytes(EncodeEvent(message), peer_id,
                           MultiplayerPeer::TRANSFER_MODE_RELIABLE);
}

void Main::OnPeerConnected(int64_t peer_id) {
  UtilityFunctions::print(vformat("Main: peer %d connected", peer_id));
  clients_[static_cast<int>(peer_id)] = ClientConnection();
  FpsCharacter* player =
      SpawnPlayer(static_cast<int>(peer_id), NextSpawnTransform());
  if (player) {
    // The client's look starts where the spawn faces.
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
  for (uint32_t i = 0; i < count; i++) {
    uint32_t sequence = message.sequence - (count - 1 - i);
    if (SequenceNewer(sequence, connection.last_received)) {
      connection.queue.push_back({sequence, message.commands[i]});
      connection.last_received = sequence;
    }
  }
  while (connection.queue.size() > kMaxQueuedCommands) {
    connection.queue.pop_front();
  }
}

void Main::OnSnapshot(const PackedByteArray& bytes) {
  SnapshotMessage snapshot;
  if (!DecodeSnapshot(bytes, &snapshot)) {
    return;
  }
  // Unreliable: an older one may arrive after a newer.
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
  } else if (message.type == EventType::kKill) {
    hud_->AddScore(100);
  }
}

void Main::OnWeaponHit(Object* collider, int64_t peer_id) {
  if (peer_id != local_peer_id_) {
    SendEvent(static_cast<int>(peer_id), EventType::kHit);
  } else if (hud_) {
    hud_->ShowHitmarker();
  }
}

void Main::OnWeaponKilled(Object* target, int64_t peer_id) {
  if (peer_id != local_peer_id_) {
    SendEvent(static_cast<int>(peer_id), EventType::kKill);
  } else if (hud_) {
    hud_->AddScore(100);
  }
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
