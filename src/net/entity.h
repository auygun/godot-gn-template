#ifndef SRC_NET_ENTITY_H_
#define SRC_NET_ENTITY_H_

#include <godot_cpp/variant/vector3.hpp>

#include "src/net/replicate.h"
#include "src/net/types.h"

namespace godot {

class Node3D;

namespace net {

// A replicated object, usually a node: the NetSession reads and writes its
// state, simulates it under its owner's commands, and on clients draws it
// from the server's snapshots. Games derive from TypedEntity, which
// unwraps the boxes.
//
// The server registers each entity (NetSession::AddEntity); clients get
// them from the snapshots (SessionDelegate::SpawnEntity), but for those
// placed in the scene, which every side registers in the same order.
class Entity {
 public:
  // The game state: everything Simulate reads and writes (of the struct
  // registered for the entity's type).
  virtual void CaptureBox(Box* state) const = 0;
  // Set the game state, without effects (to rewind a prediction).
  virtual void RestoreBox(const Box& state) = 0;
  // Draw a state the server sent (or the interpolator blended): restore
  // it, and play the effects of what changed.
  virtual void ApplyBox(const Box& state) = 0;
  // One tick under the owner's command (owned entities only): the server
  // simulates them all, a client predicts its own.
  virtual void SimulateBox(const Box& command, const SimContext& context) = 0;
  // On the owner's machine: the command from its devices.
  virtual void GatherBox(Box* command) = 0;
  // On the server, until the owner's first command arrives: what to
  // simulate meanwhile (e.g. standing still, looking where it faces).
  virtual void IdleBox(Box* command) const = 0;

  // On a client, its own entity: a prediction was corrected, and the
  // entity is now error (old minus new position) from where it was drawn;
  // or the correction changed a kTeleport field (a respawn), and it
  // shouldn't be drawn sliding there.
  virtual void OnCorrected(const Vector3& error) {}
  virtual void OnTeleported() {}
  // Where the entity is, for the correction's error.
  virtual Vector3 Position() const { return Vector3(); }
  // The body shots test against in the past (NetSession::Raycast), with
  // its hitbox from its CollisionShape3D; null for none. active: whether it
  // can be hit now (e.g. not dead).
  virtual Node3D* HitboxBody(bool* active) { return nullptr; }

 protected:
  virtual ~Entity() = default;
};

// The command of entities nobody commands.
struct NoCommand {
  template <class V>
  void Replicate(V& v) {}
};

template <class State, class Command = NoCommand>
class TypedEntity : public Entity {
 public:
  virtual State CaptureState() const = 0;
  virtual void ApplyState(const State& state) = 0;
  virtual void RestoreState(const State& state) { ApplyState(state); }
  virtual void Simulate(const Command& command, const SimContext& context) {}
  virtual Command GatherInput() { return Command(); }
  virtual Command IdleInput() const { return Command(); }

  void CaptureBox(Box* state) const final { *state = Box::Of(CaptureState()); }
  void RestoreBox(const Box& state) final { RestoreState(state.As<State>()); }
  void ApplyBox(const Box& state) final { ApplyState(state.As<State>()); }
  void SimulateBox(const Box& command, const SimContext& context) final {
    Simulate(command.As<Command>(), context);
  }
  void GatherBox(Box* command) final { *command = Box::Of(GatherInput()); }
  void IdleBox(Box* command) const final { *command = Box::Of(IdleInput()); }
};

}  // namespace net
}  // namespace godot

#endif  // SRC_NET_ENTITY_H_
