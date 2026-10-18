#ifndef SRC_NET_SCHEMA_H_
#define SRC_NET_SCHEMA_H_

#include <cstdint>
#include <vector>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include "src/net/replicate.h"
#include "src/net/types.h"

namespace godot::net {

class Entity;

// Handles an RPC's payload. context: what the game passed to Schema::Rpc.
using RpcHandler = void (*)(void* context, PeerId from, const Box& payload);
// A spawned scene's root as the Entity it is (null if it isn't one).
using EntityCast = Entity* (*)(Node* node);

// What a game replicates: its entity types (their states), its command
// type and its RPCs, in an order every side registers the same. Peers
// compare hash() when they connect.
class Schema {
 public:
  Schema();
  ~Schema();

  // The command the entities a peer owns are simulated with.
  template <class C>
  void SetCommand() {
    command_ = &LayoutOf<C>();
  }
  // An entity type whose node is a T (a Node that derives from
  // TypedEntity<S, ...>), with the scene NetSession::Spawn instantiates (on
  // the server, and on each client when the entity first shows up in a
  // snapshot) under the node at parent (relative to the session's parent).
  // Without a scene, the type is only for entities placed in the scene
  // (NetSession::AddEntity). Returns its TypeId.
  template <class T, class S>
  TypeId AddEntityType(const Ref<PackedScene>& scene = Ref<PackedScene>(),
                       const NodePath& parent = NodePath()) {
    EntityCast cast = [](Node* node) -> Entity* {
      return Object::cast_to<T>(node);
    };
    entity_types_.push_back({&LayoutOf<S>(), scene, parent, cast});
    return static_cast<TypeId>(entity_types_.size() - 1);
  }
  // A type for any Node3D scene, without game code: the session replicates
  // its transform (NodeEntity). On clients, a physics body is frozen
  // (kinematic): it moves only as the server's does.
  TypeId AddNodeType(const Ref<PackedScene>& scene, const NodePath& parent);
  // A reliable message with payload P, handled on the receiving side; one
  // RPC per payload type.
  template <class P>
  void AddRpc(RpcHandler handler, void* context) {
    rpcs_.push_back({&LayoutOf<P>(), handler, context});
  }

  const Layout* command() const { return command_; }
  const Layout* entity_type(TypeId type) const {
    return type < entity_types_.size() ? entity_types_[type].layout : nullptr;
  }
  // The type's scene (null if it has none), the path of the node spawned
  // entities go under, and whether it is a plain node (AddNodeType).
  Ref<PackedScene> entity_scene(TypeId type) const {
    return entity_types_[type].scene;
  }
  const NodePath& entity_parent(TypeId type) const {
    return entity_types_[type].parent;
  }
  // Null for a plain node type (AddNodeType).
  EntityCast entity_cast(TypeId type) const { return entity_types_[type].cast; }
  // The RPC for the payload type, or -1.
  int FindRpc(const void* key) const;
  const Layout* rpc_layout(int rpc) const { return rpcs_[rpc].layout; }
  void HandleRpc(int rpc, PeerId from, const Box& payload) const;
  size_t rpc_count() const { return rpcs_.size(); }

  uint32_t hash() const;

 private:
  struct Rpc {
    const Layout* layout = nullptr;
    RpcHandler handler = nullptr;
    void* context = nullptr;
  };

  struct EntityType {
    const Layout* layout = nullptr;
    Ref<PackedScene> scene;
    NodePath parent;
    EntityCast cast = nullptr;
  };

  const Layout* command_ = nullptr;
  std::vector<EntityType> entity_types_;
  std::vector<Rpc> rpcs_;
};

}  // namespace godot::net

#endif  // SRC_NET_SCHEMA_H_
