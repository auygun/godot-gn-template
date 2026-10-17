#ifndef SRC_NET_SCHEMA_H_
#define SRC_NET_SCHEMA_H_

#include <cstdint>
#include <vector>

#include "src/net/replicate.h"
#include "src/net/types.h"

namespace godot::net {

// Handles an RPC's payload. context: what the game passed to Schema::Rpc.
using RpcHandler = void (*)(void* context, PeerId from, const Box& payload);

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
  // An entity type, with its state struct; returns its TypeId.
  template <class S>
  TypeId AddEntityType() {
    entity_types_.push_back(&LayoutOf<S>());
    return static_cast<TypeId>(entity_types_.size() - 1);
  }
  // A reliable message with payload P, handled on the receiving side; one
  // RPC per payload type.
  template <class P>
  void AddRpc(RpcHandler handler, void* context) {
    rpcs_.push_back({&LayoutOf<P>(), handler, context});
  }

  const Layout* command() const { return command_; }
  const Layout* entity_type(TypeId type) const {
    return type < entity_types_.size() ? entity_types_[type] : nullptr;
  }
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

  const Layout* command_ = nullptr;
  std::vector<const Layout*> entity_types_;
  std::vector<Rpc> rpcs_;
};

}  // namespace godot::net

#endif  // SRC_NET_SCHEMA_H_
