#include "src/net/schema.h"

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
  for (const Layout* layout : entity_types_) {
    hash = Mix(hash, layout->hash);
  }
  hash = Mix(hash, static_cast<uint32_t>(rpcs_.size()));
  for (const Rpc& rpc : rpcs_) {
    hash = Mix(hash, rpc.layout->hash);
  }
  return hash;
}

}  // namespace godot::net
