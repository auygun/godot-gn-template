#ifndef SRC_IMPACT_H_
#define SRC_IMPACT_H_

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

namespace godot {

class Decal;

// A bullet impact: one-shot spark/dust particles (start on entering the
// tree) and a hole decal that fades out, then frees the node. Local +Y is
// the surface normal.
class Impact : public Node3D {
  GDCLASS(Impact, Node3D);

 public:
  Impact();
  ~Impact() override;

  void _ready() override;
  void _process(double delta) override;

 protected:
  static void _bind_methods();

 private:
  // How long the hole stays at full strength, then how long it takes to
  // fade out, in seconds.
  static constexpr float kHoleTime = 10.0f;
  static constexpr float kFadeTime = 2.0f;

  // The Hole decal.
  Decal* hole_ = nullptr;
  float age_ = 0.0f;
};

}  // namespace godot

#endif  // SRC_IMPACT_H_
