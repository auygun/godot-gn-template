#include "src/net/node_entity.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rigid_body3d.hpp>

namespace godot::net {

NodeEntity::NodeEntity(Node3D* node) : node_(node) {}

NodeEntity::~NodeEntity() = default;

NodeState NodeEntity::CaptureState() const {
  Transform3D transform = node_->get_global_transform();
  NodeState state;
  state.position = transform.origin;
  state.rotation = transform.basis.orthonormalized().get_quaternion();
  return state;
}

void NodeEntity::ApplyState(const NodeState& state) {
  Vector3 scale = node_->get_global_transform().basis.get_scale();
  node_->set_global_transform(
      Transform3D(Basis(state.rotation).scaled_local(scale), state.position));
  if (!has_state_) {
    has_state_ = true;
    node_->reset_physics_interpolation();
  }
}

void NodeEntity::OnRemoteAuthority() {
  RigidBody3D* body = Object::cast_to<RigidBody3D>(node_);
  if (body) {
    body->set_freeze_mode(RigidBody3D::FREEZE_MODE_KINEMATIC);
    body->set_freeze_enabled(true);
  }
}

}  // namespace godot::net
