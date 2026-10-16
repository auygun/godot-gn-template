#ifndef SRC_MAIN_H_
#define SRC_MAIN_H_

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/scene_multiplayer.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

#include "src/net_protocol.h"
#include "src/player_input.h"

namespace godot {

class FpsCharacter;
class Hud;
class Node3D;
class Target;

// The game session: spawns the players and runs every physics tick.
//
// The server is authoritative: it simulates every player from their
// commands (the local player's from its devices, the clients' from the
// network) and sends the game state back each tick. A client sends its
// commands and draws the state it receives. The mode comes from the
// command line, after "--":
//
//   (nothing)               offline: a server without the network
//   --host [port]           listen server, with a local player
//   --server [port]         dedicated server, no local player (--headless)
//   --join <address> [port] client
class Main : public Node {
  GDCLASS(Main, Node);

 public:
  Main();
  ~Main() override;

  void _ready() override;
  void _exit_tree() override;
  void _physics_process(double delta) override;

  void set_player_scene(const Ref<PackedScene>& scene) {
    player_scene_ = scene;
  }
  Ref<PackedScene> player_scene() const { return player_scene_; }

 protected:
  static void _bind_methods();

 private:
  enum class Mode { kOffline, kHost, kDedicated, kClient };

  // Peer id of the server (and of the player at an offline or host
  // machine).
  static constexpr int kServerPeerId = 1;
  static constexpr int kMaxClients = 15;
  // Commands a server keeps queued for a client; past this it drops the
  // oldest, so a burst of late packets doesn't become lasting lag.
  static constexpr size_t kMaxQueuedCommands = 8;

  // On the server, per client.
  struct ClientConnection {
    struct QueuedCommand {
      uint32_t sequence = 0;
      PlayerInput command;
    };
    std::deque<QueuedCommand> queue;
    // The newest command received, and the last simulated (repeated while
    // the queue is empty).
    uint32_t last_received = 0;
    uint32_t last_simulated = 0;
    PlayerInput last_command;
  };

  // Read the mode (and address and port) from the command line. False if
  // it doesn't parse.
  bool ParseCommandLine();
  // Start listening or connecting. False on failure.
  bool StartNetwork();

  // Instance the player scene for peer_id at transform, under Players.
  FpsCharacter* SpawnPlayer(int peer_id, const Transform3D& transform);
  void DespawnPlayer(int peer_id);
  // The next spawn point, round robin.
  Transform3D NextSpawnTransform();

  void ServerTick(float dt);
  void ClientTick();
  void SendSnapshots();
  void ApplySnapshot(const SnapshotMessage& snapshot);
  void SendInput(const PlayerInput& command);
  void SendEvent(int peer_id, EventType type);

  void OnPeerConnected(int64_t peer_id);
  void OnPeerDisconnected(int64_t peer_id);
  void OnPeerPacket(int64_t peer_id, const PackedByteArray& bytes);
  void OnConnectedToServer();
  void OnConnectionFailed();
  void OnServerDisconnected();
  void OnInput(int peer_id, const PackedByteArray& bytes);
  void OnSnapshot(const PackedByteArray& bytes);
  void OnEvent(const PackedByteArray& bytes);
  // From the players' weapons, bound to the shooter's peer id.
  void OnWeaponHit(Object* collider, int64_t peer_id);
  void OnWeaponKilled(Object* target, int64_t peer_id);
  void OnAmmoChanged(int ammo, int reserve, bool reloading);

  bool is_server() const { return mode_ != Mode::kClient; }

  Ref<PackedScene> player_scene_;
  Hud* hud_ = nullptr;
  Node* players_root_ = nullptr;
  // The scene's, in order: snapshots refer to them by index.
  std::vector<Target*> targets_;
  std::vector<Node3D*> spawn_points_;
  size_t next_spawn_ = 0;

  Mode mode_ = Mode::kOffline;
  String address_;
  int port_ = kDefaultPort;
  Ref<SceneMultiplayer> multiplayer_;
  // The player at this machine (0: none, as on a dedicated server).
  int local_peer_id_ = 0;
  std::map<int, FpsCharacter*> players_;

  // Server.
  std::map<int, ClientConnection> clients_;
  uint32_t tick_ = 0;

  // Client. The newest snapshot, applied on the next tick.
  SnapshotMessage snapshot_;
  bool has_snapshot_ = false;
  bool snapshot_pending_ = false;
  // The last commands sent, by sequence number.
  uint32_t sequence_ = 0;
  std::array<PlayerInput, kMaxInputCommands> recent_commands_;
};

}  // namespace godot

#endif  // SRC_MAIN_H_
