#include "src/net/schema.h"

#include "src/net/node_entity.h"

namespace godot::net {

namespace {

uint32_t Mix(uint32_t hash, uint32_t value) {
  for (int i = 0; i < 4; i++) {
    hash ^= (value >> (i * 8)) & 0xff;
    hash *= 16777619u;
  }
  return hash;
}

}  // namespace

Schema::Schema() = default;

Schema::~Schema() = default;

TypeId Schema::AddNodeType(const Ref<PackedScene>& scene,
                           const NodePath& parent) {
  entity_types_.push_back({&LayoutOf<NodeState>(), scene, parent, nullptr});
  return static_cast<TypeId>(entity_types_.size() - 1);
}

bool Schema::MatchSceneNode(Node* node, TypeId* type) const {
  String scene_path = node->get_scene_file_path();
  for (size_t i = 0; i < entity_types_.size(); i++) {
    const EntityType& entry = entity_types_[i];
    // Plain node types by their scene: by class, every Node3D would match.
    bool match = entry.cast
                     ? entry.cast(node) != nullptr
                     : entry.scene.is_valid() && !scene_path.is_empty() &&
                           scene_path == entry.scene->get_path();
    if (match) {
      *type = static_cast<TypeId>(i);
      return true;
    }
  }
  return false;
}

int Schema::FindRpc(const void* key) const {
  for (size_t i = 0; i < rpcs_.size(); i++) {
    if (rpcs_[i].layout->key == key) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void Schema::HandleRpc(int rpc, PeerId from, const Box& payload) const {
  const Rpc& entry = rpcs_[rpc];
  if (entry.handler) {
    entry.handler(entry.context, from, payload);
  }
}

uint32_t Schema::hash() const {
  uint32_t hash = 2166136261u;
  hash = Mix(hash, command_ ? command_->hash : 0);
  hash = Mix(hash, static_cast<uint32_t>(entity_types_.size()));
  for (const EntityType& type : entity_types_) {
    hash = Mix(hash, type.layout->hash);
    // Which scene: a path both sides have.
    String path = type.scene.is_valid() ? type.scene->get_path() : String();
    CharString utf8 = path.utf8();
    for (int i = 0; i < utf8.length(); i++) {
      hash = Mix(hash, static_cast<uint8_t>(utf8[i]));
    }
    hash = Mix(hash, type.cast ? 1 : 0);
  }
  hash = Mix(hash, static_cast<uint32_t>(rpcs_.size()));
  for (const Rpc& rpc : rpcs_) {
    hash = Mix(hash, rpc.layout->hash);
  }
  return hash;
}

}  // namespace godot::net
