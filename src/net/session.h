#ifndef SRC_NET_SESSION_H_
#define SRC_NET_SESSION_H_

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include <godot_cpp/classes/multiplayer_peer.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/scene_multiplayer.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

#include "src/net/client_prediction.h"
#include "src/net/entity.h"
#include "src/net/lag_compensation.h"
#include "src/net/protocol.h"
#include "src/net/schema.h"
#include "src/net/snapshot_interpolator.h"
#include "src/net/types.h"

namespace godot::net {

// What the game does for the session.
class SessionDelegate {
 public:
  // On a client: an entity the server has and this client doesn't yet,
  // with its first state (as this client receives it). Returns it, or null
  // if it can't (the session asks again with the next snapshot).
  virtual Entity* SpawnEntity(EntityId id,
                              TypeId type,
                              PeerId owner,
                              const Box& state) = 0;
  // On a client: the server no longer has it. On the server, after
  // NetSession::RemoveEntity. The session forgets it right after.
  virtual void DespawnEntity(EntityId id, Entity* entity) = 0;
  // On the server: a client connected (with the same Schema), or left. The
  // session removes the entities it owned after OnPeerLeft (calling
  // DespawnEntity for each).
  virtual void OnPeerJoined(PeerId peer) {}
  virtual void OnPeerLeft(PeerId peer) {}
  // On the server, each tick before the commands are simulated.
  virtual void BeforeServerTick(float dt) {}

 protected:
  virtual ~SessionDelegate() = default;
};

// A networked game session, server-authoritative: the server simulates
// every entity (those its clients own under their commands) and sends each
// client snapshots of the world; a client predicts its own entities and
// draws the rest from the snapshots, a little in the past. Offline is a
// server without the network: single player runs the server's code.
//
// The game registers what it replicates in schema(), sets the delegate,
// registers its scene entities, then calls Start (in _ready). The mode
// comes from the command line, after "--":
//
//   (nothing)               offline
//   --host [port]           listen server, with a local player
//   --server [port]         dedicated server, no local player (--headless)
//   --join <address> [port] client
//
// followed by --lag <ms> and --loss <percent> to simulate a bad network,
// and --netstats to print the traffic every few seconds.
//
// Runs its physics tick before every other node's (priority -1): the
// entities' own _physics_process sees the tick's result.
class NetSession : public Node {
  GDCLASS(NetSession, Node);

 public:
  enum class Mode { kOffline, kHost, kDedicated, kClient };

  static constexpr int kDefaultPort = 24680;

  NetSession();
  ~NetSession() override;

  void _ready() override;
  void _exit_tree() override;
  void _physics_process(double delta) override;

  Schema& schema() { return schema_; }
  void set_delegate(SessionDelegate* delegate) { delegate_ = delegate; }

  // Parse the command line and start listening or connecting. False (and
  // an error printed) on failure: the game should quit.
  bool Start();

  // On the server, or on every side for an entity placed in the scene
  // (registered in the same order on each, before Start): a new entity.
  // owner: the peer whose commands it is simulated with (kNoOwner: none,
  // it simulates itself). Returns its id.
  EntityId AddEntity(Entity* entity, TypeId type, PeerId owner);
  // On the server: the entity is gone (the delegate's DespawnEntity is
  // called).
  void RemoveEntity(EntityId id);
  Entity* FindEntity(EntityId id) const;
  PeerId OwnerOf(EntityId id) const;
  // The id of a registered entity, or 0.
  EntityId IdOf(const Entity* entity) const;

  // A reliable message to peer (on the server: kServerPeerId for its own
  // player, handled right away; on a client, the server). Its payload type
  // must have been registered with Schema::AddRpc.
  template <class P>
  void Rpc(PeerId peer, const P& payload) {
    SendRpc(peer, Box::Of(payload));
  }

  // On the server: the nearest shootable body (Entity::HitboxBody) the ray
  // from origin along direction (normalized), at most max_distance long,
  // hits as the bodies were at view_tick (at most kMaxRewindTicks back);
  // never ignore. Null if none.
  Node3D* Raycast(double view_tick,
                  const Vector3& origin,
                  const Vector3& direction,
                  float max_distance,
                  const Node3D* ignore,
                  Vector3* position,
                  Vector3* normal) const;

  Mode mode() const { return mode_; }
  bool is_server() const { return mode_ != Mode::kClient; }
  // This machine's peer id: kServerPeerId on a server, the client's once
  // connected (0 before).
  PeerId local_peer() const { return local_peer_; }
  // Whether this machine has a player (not a dedicated server).
  bool has_local_player() const { return mode_ != Mode::kDedicated; }
  // The connected clients (on the server).
  std::vector<PeerId> peers() const;

 protected:
  static void _bind_methods();

 private:
  static constexpr int kMaxClients = 15;
  // The server sends a snapshot every this many ticks.
  static constexpr uint32_t kSnapshotInterval = 2;
  // How far in the past a client's shots may be tested, in ticks: its
  // view's delay (SnapshotInterpolator) plus up to 200 ms of latency. A
  // client with more lag has to lead its targets.
  static constexpr double kMaxRewindTicks = 6.0 + 12.0;
  // How far ahead of the next command to simulate the server keeps a
  // client's commands.
  static constexpr uint32_t kMaxQueuedCommands = 32;
  // Simulated network conditions (--lag ms, --loss percent), for testing
  // prediction: every packet this machine sends is held back or dropped.
  static constexpr int kMaxLagMs = 1000;
  // The snapshots kept as baselines (each side its own): a second's worth
  // at 30 per second.
  static constexpr size_t kSnapshotHistory = 32;
  // --netstats: print ENet's traffic counters (after compression) every
  // kNetStatsInterval seconds.
  static constexpr float kNetStatsInterval = 5.0f;

  struct EntityRecord {
    Entity* entity = nullptr;
    TypeId type = 0;
    PeerId owner = kNoOwner;
  };

  // On the server, per client. Each tick simulates the client's next
  // command by sequence number; if it hasn't arrived, the last command
  // again in its place (and the real one is dropped when it comes).
  struct ClientConnection {
    // Whether its Schema matched (kHello): until then it gets nothing.
    bool joined = false;
    // Received commands not yet simulated, by sequence number.
    std::map<uint32_t, TimedCommand> queue;
    // The next sequence number to simulate; 0 until the first command
    // arrives.
    uint32_t next_sequence = 0;
    // The newest sequence number received.
    uint32_t last_received = 0;
    // The newest snapshot the client has decoded (0: none), the baseline
    // for the next.
    uint32_t snapshot_ack = 0;
    // The last sequence number simulated (0: none), and its command.
    uint32_t last_simulated = 0;
    TimedCommand last_command;
  };

  struct DelayedPacket {
    uint64_t send_at_usec = 0;
    PackedByteArray bytes;
    PeerId peer = 0;
    MultiplayerPeer::TransferMode mode =
        MultiplayerPeer::TRANSFER_MODE_RELIABLE;
  };

  bool ParseCommandLine();
  bool StartNetwork();

  void ServerTick(float dt);
  void ClientTick(float dt);
  // The command of a client's entity this tick (repeating the last if the
  // next hasn't arrived).
  TimedCommand NextCommand(PeerId peer, Entity* entity);
  void RecordHitboxes();
  void SendSnapshots();
  void SendInput();
  void SendRpc(PeerId peer, const Box& payload);
  void ReconcileOwned(const Snapshot& snapshot, float dt);
  void ApplyRemote(const Snapshot& snapshot);
  // On a client: an entity of the snapshot it doesn't have yet.
  Entity* SpawnFromSnapshot(const SnapshotEntity& entry);
  void Despawn(EntityId id);
  // send_bytes through the simulated lag and loss.
  void Send(const PackedByteArray& bytes,
            PeerId peer,
            MultiplayerPeer::TransferMode mode);
  void FlushDelayedPackets();
  void PrintNetStats(float dt);

  const Snapshot* FindSnapshot(uint32_t tick) const;
  void KeepSnapshot(const Snapshot& snapshot);
#ifdef DEBUG_ENABLED
  // Debug builds: decode what was just encoded and compare.
  void CheckSnapshotEncoding(const PackedByteArray& bytes,
                             const Snapshot& snapshot,
                             const Snapshot* baseline,
                             PeerId peer);
#endif

  void OnPeerConnected(int64_t peer);
  void OnPeerDisconnected(int64_t peer);
  void OnPeerPacket(int64_t peer, const PackedByteArray& bytes);
  void OnConnectedToServer();
  void OnConnectionFailed();
  void OnServerDisconnected();
  void OnHello(PeerId peer, const PackedByteArray& bytes);
  void OnInput(PeerId peer, const PackedByteArray& bytes);
  void OnSnapshot(const PackedByteArray& bytes);
  void OnRpc(PeerId peer, const PackedByteArray& bytes);

  Schema schema_;
  SessionDelegate* delegate_ = nullptr;
  bool started_ = false;

  Mode mode_ = Mode::kOffline;
  String address_;
  int port_ = kDefaultPort;
  Ref<SceneMultiplayer> multiplayer_;
  PeerId local_peer_ = 0;

  // By id, ascending.
  std::map<EntityId, EntityRecord> entities_;
  // Ids up to this one are the scene's (the same on every side).
  EntityId next_id_ = 1;

  std::array<Snapshot, kSnapshotHistory> snapshot_history_;

  // Server.
  std::map<PeerId, ClientConnection> clients_;
  uint32_t tick_ = 0;
  LagCompensation lag_compensation_;

  // Client. The newest snapshot, reconciled on the next tick.
  Snapshot snapshot_;
  bool has_snapshot_ = false;
  bool snapshot_pending_ = false;
  // Its own entity (the one it predicts); 0 until the server spawns it.
  EntityId own_entity_ = 0;
  ClientPrediction prediction_;
  SnapshotInterpolator interpolator_;

  // Simulated network conditions.
  int lag_ms_ = 0;
  int loss_percent_ = 0;
  std::deque<DelayedPacket> delayed_packets_;

  bool net_stats_ = false;
  float net_stats_timer_ = 0.0f;
  uint64_t net_stats_sent_ = 0;
  uint64_t net_stats_received_ = 0;
  uint64_t net_stats_packets_ = 0;
};

}  // namespace godot::net

#endif  // SRC_NET_SESSION_H_
