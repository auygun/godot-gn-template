#ifndef SRC_MAIN_H_
#define SRC_MAIN_H_

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include <godot_cpp/classes/multiplayer_peer.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/scene_multiplayer.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

#include "src/client_prediction.h"
#include "src/net_protocol.h"
#include "src/player_input.h"
#include "src/snapshot_interpolator.h"

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
//
// followed by --lag <ms> and --loss <percent> to simulate a bad network.
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
  // The server sends a snapshot every this many ticks.
  static constexpr uint32_t kSnapshotInterval = 2;
  // How far ahead of the next command to simulate the server keeps a
  // client's commands.
  static constexpr uint32_t kMaxQueuedCommands = 32;
  // Simulated network conditions (--lag ms, --loss percent), for testing
  // prediction: every packet this machine sends is held back or dropped.
  static constexpr int kMaxLagMs = 1000;

  // On the server, per client. Each tick simulates the client's next
  // command by sequence number; if it hasn't arrived, the last command
  // again in its place (and the real one is dropped when it comes).
  struct ClientConnection {
    // Received commands not yet simulated, by sequence number.
    std::map<uint32_t, PlayerInput> queue;
    // The next sequence number to simulate; 0 until the first command
    // arrives.
    uint32_t next_sequence = 0;
    // The newest sequence number received.
    uint32_t last_received = 0;
    // The last sequence number simulated (0: none), and its command.
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
  void ClientTick(float dt);
  void SendSnapshots();
  // The newest snapshot, for our own player's prediction.
  void ReconcileLocalPlayer(const SnapshotMessage& snapshot, float dt);
  // A state drawn from the snapshots: everyone else, and the targets.
  void ApplyRemoteState(const SnapshotMessage& state);
  void SendInput();
  void SendEvent(int peer_id, EventType type);
  // send_bytes through the simulated lag and loss.
  void Send(const PackedByteArray& bytes,
            int peer_id,
            MultiplayerPeer::TransferMode mode);
  void FlushDelayedPackets();

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

  // Client. The newest snapshot, reconciled on the next tick.
  SnapshotMessage snapshot_;
  bool has_snapshot_ = false;
  bool snapshot_pending_ = false;
  ClientPrediction prediction_;
  SnapshotInterpolator interpolator_;

  // Simulated network conditions.
  struct DelayedPacket {
    uint64_t send_at_usec = 0;
    PackedByteArray bytes;
    int peer_id = 0;
    MultiplayerPeer::TransferMode mode =
        MultiplayerPeer::TRANSFER_MODE_RELIABLE;
  };
  int lag_ms_ = 0;
  int loss_percent_ = 0;
  std::deque<DelayedPacket> delayed_packets_;
};

}  // namespace godot

#endif  // SRC_MAIN_H_
