#ifndef SRC_NET_NODE_ENTITY_H_
#define SRC_NET_NODE_ENTITY_H_

#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "src/net/entity.h"
#include "src/net/replicate.h"

namespace godot {

class Node3D;

namespace net {

// The state of a plain node (Schema::AddNodeType): its transform, without
// scale.
struct NodeState {
  Vector3 position;
  Quaternion rotation;

  template <class V>
  void Replicate(V& v) {
    v(position, Position(), kAll | kBlend);
    v(rotation, Rotation(), kAll | kBlend);
  }
};

// Replicates a Node3D the game has no code for: on the server it moves on
// its own (e.g. a RigidBody3D under physics), and clients draw its
// transform. Owned by the NetSession, which frees it with the node.
class NodeEntity : public TypedEntity<NodeState> {
 public:
  // The node must outlive this (NetSession frees both together).
  NodeEntity(Node3D* node, bool server);
  ~NodeEntity() override;

  NodeState CaptureState() const override;
  void ApplyState(const NodeState& state) override;
  Vector3 Position() const override;

  Node3D* node() const { return node_; }

 private:
  Node3D* node_;
  // Whether a state was applied: the first places it without
  // interpolating from the scene's.
  bool has_state_ = false;
};

}  // namespace net
}  // namespace godot

#endif  // SRC_NET_NODE_ENTITY_H_
