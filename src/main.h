#ifndef SRC_MAIN_H_
#define SRC_MAIN_H_

#include <map>
#include <vector>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

#include "src/lag_compensation.h"
#include "src/net/session.h"
#include "src/weapon.h"

namespace godot {

class FpsCharacter;
class Hud;
class Node3D;
class Target;

// The game: a deathmatch. The networking is the NetSession's (src/net/,
// the "NetSession" child): Main declares what it replicates (Schema), spawns
// the players it asks for, and keeps the rules (spawn points, scores,
// respawns) and the HUD.
class Main : public Node, public ShotResolver, public net::SessionDelegate {
  GDCLASS(Main, Node);

 public:
  Main();
  ~Main() override;

  void _ready() override;
  void _physics_process(double delta) override;

  void set_player_scene(const Ref<PackedScene>& scene) {
    player_scene_ = scene;
  }
  Ref<PackedScene> player_scene() const { return player_scene_; }
  // A physics prop the server drops in a few of (any Node3D scene, without
  // game code: the session replicates its transform).
  void set_crate_scene(const Ref<PackedScene>& scene) { crate_scene_ = scene; }
  Ref<PackedScene> crate_scene() const { return crate_scene_; }

  // ShotResolver, on the server: shots are tested against the players and
  // targets as the shooter saw them.
  TypedArray<RID> ShootableBodies() override;
  Node3D* ResolveShot(const Node3D* shooter,
                      double view_tick,
                      const Vector3& origin,
                      const Vector3& direction,
                      float max_distance,
                      Vector3* position,
                      Vector3* normal) override;

  // net::SessionDelegate.
  void OnSpawning(Node* node,
                  net::EntityId id,
                  net::TypeId type,
                  net::PeerId owner) override;
  void OnSpawned(Node* node,
                 net::EntityId id,
                 net::TypeId type,
                 net::PeerId owner) override;
  void OnDespawned(Node* node, net::EntityId id) override;
  void OnPeerJoined(net::PeerId peer) override;
  void BeforeServerTick(float dt) override;
  // Remember where the shootable bodies are this tick (LagCompensation).
  void AfterServerTick(uint32_t tick) override;

 protected:
  static void _bind_methods();

 private:
  // How long a killed player waits to respawn, in seconds.
  static constexpr float kRespawnDelay = 3.0f;
  // How far in the past a client's shots may be tested, in ticks: its
  // view's delay (SnapshotInterpolator) plus up to 200 ms of latency. A
  // client with more lag has to lead its targets. Within LagCompensation's
  // history.
  static constexpr double kMaxRewindTicks = 6.0 + 12.0;

  // The RPCs: from the server to the client of the player concerned.
  struct HitEvent {  // its shot hit something
    template <class V>
    void Replicate(V& v) {}
  };
  struct KillEvent {  // ...and killed another player
    template <class V>
    void Replicate(V& v) {}
  };
  struct DamagedEvent {  // it was hit
    template <class V>
    void Replicate(V& v) {}
  };
  static void OnHitEvent(void* self, net::PeerId from, const net::Box&);
  static void OnDamagedEvent(void* self, net::PeerId from, const net::Box&);

  // Where the server drops the crates, near the first spawn point.
  static constexpr int kCrateCount = 4;

  // The next spawn point, round robin.
  Transform3D NextSpawnTransform();
  // The spawn point farthest from the living players (but for exclude).
  Transform3D SafestSpawnTransform(const FpsCharacter* exclude);
  FpsCharacter* PlayerOf(net::PeerId peer) const;

  // From the players' weapons, bound to the shooter's peer id.
  void OnWeaponHit(Object* collider, int64_t peer);
  void OnWeaponKilled(Object* victim, int64_t peer);
  // A player lost health (bound to its peer id).
  void OnPlayerDamaged(int64_t peer);
  // Show the local player's health and score on the HUD.
  void UpdateHud();
  void OnAmmoChanged(int ammo, int reserve, bool reloading);

  Ref<PackedScene> player_scene_;
  Ref<PackedScene> crate_scene_;
  net::NetSession* session_ = nullptr;
  net::TypeId player_type_ = 0;
  Hud* hud_ = nullptr;
  std::vector<Target*> targets_;
  std::vector<Node3D*> spawn_points_;
  size_t next_spawn_ = 0;

  // By peer id (the owner of each player entity).
  std::map<net::PeerId, FpsCharacter*> players_;
  // On the server: seconds until each dead player respawns, by peer id.
  std::map<net::PeerId, float> respawn_timers_;
  // On the server: the bodies' hitboxes on recent ticks.
  LagCompensation lag_compensation_;
};

}  // namespace godot

#endif  // SRC_MAIN_H_
