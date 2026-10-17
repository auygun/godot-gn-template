#include "src/main.h"

#include <algorithm>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "src/fps_character.h"
#include "src/hud.h"
#include "src/target.h"
#include "src/weapon.h"

namespace godot {

Main::Main() = default;

Main::~Main() = default;

void Main::_ready() {
  hud_ = Object::cast_to<Hud>(get_node_or_null("Hud"));
  players_root_ = get_node_or_null("Players");
  if (!players_root_) {
    players_root_ = this;
  }
  session_ = Object::cast_to<net::NetSession>(get_node_or_null("NetSession"));
  if (!session_) {
    UtilityFunctions::push_error("Main: no NetSession child");
    return;
  }
  if (!player_scene_.is_valid()) {
    UtilityFunctions::push_error("Main: player_scene is not set");
    return;
  }

  // What the game replicates; the same on every side.
  net::Schema& schema = session_->schema();
  schema.SetCommand<PlayerInput>();
  player_type_ = schema.AddEntityType<PlayerState>();
  target_type_ = schema.AddEntityType<TargetState>();
  schema.AddRpc<HitEvent>(&Main::OnHitEvent, this);
  schema.AddRpc<KillEvent>(nullptr, nullptr);
  schema.AddRpc<DamagedEvent>(&Main::OnDamagedEvent, this);
  session_->set_delegate(this);

  // The scene's targets, in order: the same ids on every side.
  Node* targets = get_node_or_null("Targets");
  if (targets) {
    for (int i = 0; i < targets->get_child_count(); i++) {
      Target* target = Object::cast_to<Target>(targets->get_child(i));
      if (target) {
        targets_.push_back(target);
        session_->AddEntity(target, target_type_, net::kNoOwner);
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

  if (!session_->Start()) {
    get_tree()->quit(1);
    return;
  }
  if (!session_->is_server()) {
    // The server simulates them.
    for (Target* target : targets_) {
      target->set_simulated(false);
    }
  } else if (session_->has_local_player()) {
    FpsCharacter* player =
        SpawnPlayer(net::kServerPeerId, NextSpawnTransform());
    if (player) {
      session_->AddEntity(player, player_type_, net::kServerPeerId);
    }
  }
}

void Main::_physics_process(double delta) {
  // After the NetSession's tick.
  UpdateHud();
}

FpsCharacter* Main::SpawnPlayer(net::PeerId peer,
                                const Transform3D& transform) {
  FpsCharacter* player =
      Object::cast_to<FpsCharacter>(player_scene_->instantiate());
  if (!player) {
    UtilityFunctions::push_error("Main: player_scene isn't an FpsCharacter");
    return nullptr;
  }
  bool local = peer == session_->local_peer();
  player->set_name(vformat("Player%d", peer));
  player->set_local(local);
  // Before entering the tree, where it takes this as its spawn.
  player->set_transform(transform);
  players_root_->add_child(player);
  players_[peer] = player;

  Weapon* weapon = player->weapon();
  if (session_->is_server()) {
    if (weapon) {
      weapon->SetShotResolver(this);
      weapon->connect("hit", callable_mp(this, &Main::OnWeaponHit).bind(peer));
      weapon->connect("killed",
                      callable_mp(this, &Main::OnWeaponKilled).bind(peer));
    }
    player->connect("damaged",
                    callable_mp(this, &Main::OnPlayerDamaged).bind(peer));
  }
  if (local) {
    if (weapon) {
      weapon->connect("ammo_changed", callable_mp(this, &Main::OnAmmoChanged));
      OnAmmoChanged(weapon->ammo(), weapon->reserve_ammo(), false);
    }
    UtilityFunctions::print(vformat("Main: spawned local player %d", peer));
  }
  return player;
}

net::Entity* Main::SpawnEntity(net::EntityId id,
                               net::TypeId type,
                               net::PeerId owner,
                               const net::Box& state) {
  const PlayerState* player_state = state.Get<PlayerState>();
  if (type != player_type_ || !player_state || players_.count(owner)) {
    return nullptr;
  }
  return SpawnPlayer(
      owner, Transform3D(Basis(Vector3(0.0f, 1.0f, 0.0f), player_state->yaw),
                         player_state->position));
}

void Main::DespawnEntity(net::EntityId id, net::Entity* entity) {
  for (auto it = players_.begin(); it != players_.end(); ++it) {
    if (it->second == entity) {
      it->second->queue_free();
      respawn_timers_.erase(it->first);
      players_.erase(it);
      return;
    }
  }
}

void Main::OnPeerJoined(net::PeerId peer) {
  FpsCharacter* player = SpawnPlayer(peer, NextSpawnTransform());
  if (player) {
    session_->AddEntity(player, player_type_, peer);
  }
}

void Main::BeforeServerTick(float dt) {
  // Respawn the dead whose wait is over, before this tick's commands.
  for (auto it = respawn_timers_.begin(); it != respawn_timers_.end();) {
    it->second -= dt;
    FpsCharacter* player = PlayerOf(it->first);
    if (!player) {
      it = respawn_timers_.erase(it);
    } else if (it->second <= 0.0f) {
      // Unless something else (a script) respawned it meanwhile.
      if (player->dead()) {
        player->RespawnAt(SafestSpawnTransform(player));
      }
      it = respawn_timers_.erase(it);
    } else {
      ++it;
    }
  }
}

FpsCharacter* Main::PlayerOf(net::PeerId peer) const {
  auto it = players_.find(peer);
  return it != players_.end() ? it->second : nullptr;
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
    for (const auto& [peer, player] : players_) {
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

TypedArray<RID> Main::ShootableBodies() {
  TypedArray<RID> bodies;
  for (const auto& [peer, player] : players_) {
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
  return session_->Raycast(view_tick, origin, direction, max_distance, shooter,
                           position, normal);
}

void Main::OnWeaponHit(Object* collider, int64_t peer) {
  session_->Rpc(static_cast<net::PeerId>(peer), HitEvent());
}

void Main::OnWeaponKilled(Object* victim, int64_t peer) {
  // Targets are only for practice.
  FpsCharacter* victim_player = Object::cast_to<FpsCharacter>(victim);
  if (!victim_player) {
    return;
  }
  net::PeerId victim_peer = 0;
  for (const auto& [id, player] : players_) {
    if (player == victim_player) {
      victim_peer = id;
    }
  }
  FpsCharacter* killer = PlayerOf(static_cast<net::PeerId>(peer));
  if (killer) {
    killer->AddKill();
  }
  victim_player->AddDeath();
  respawn_timers_[victim_peer] = kRespawnDelay;
  UtilityFunctions::print(
      vformat("Main: player %d killed player %d", peer, victim_peer));
  session_->Rpc(static_cast<net::PeerId>(peer), KillEvent());
}

void Main::OnPlayerDamaged(int64_t peer) {
  session_->Rpc(static_cast<net::PeerId>(peer), DamagedEvent());
}

// static
void Main::OnHitEvent(void* self, net::PeerId from, const net::Box&) {
  Main* main = static_cast<Main*>(self);
  if (main->hud_) {
    main->hud_->ShowHitmarker();
  }
}

// static
void Main::OnDamagedEvent(void* self, net::PeerId from, const net::Box&) {
  Main* main = static_cast<Main*>(self);
  if (main->hud_) {
    main->hud_->ShowDamage();
  }
}

void Main::UpdateHud() {
  FpsCharacter* player = session_ ? PlayerOf(session_->local_peer()) : nullptr;
  if (!hud_ || !player) {
    return;
  }
  hud_->UpdateStatus(player->health(), player->dead(), player->kills(),
                     player->deaths());
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
