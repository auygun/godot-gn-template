#include "src/impact.h"

#include <godot_cpp/classes/decal.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math_defs.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

Impact::Impact() = default;

Impact::~Impact() = default;

void Impact::_ready() {
  hole_ = Object::cast_to<Decal>(get_node_or_null("Hole"));

  // A random roll and size so neighboring holes don't look identical.
  rotate_object_local(
      Vector3(0.0f, 1.0f, 0.0f),
      static_cast<float>(UtilityFunctions::randf_range(0.0, Math::TAU)));
  if (hole_) {
    float size = static_cast<float>(UtilityFunctions::randf_range(0.8, 1.2));
    Vector3 extents = hole_->get_size();
    hole_->set_size(Vector3(extents.x * size, extents.y, extents.z * size));
  }
}

void Impact::_process(double delta) {
  age_ += static_cast<float>(delta);
  if (age_ < kHoleTime) {
    return;
  }
  float t = (age_ - kHoleTime) / kFadeTime;
  if (t >= 1.0f) {
    queue_free();
    return;
  }
  if (hole_) {
    hole_->set_modulate(Color(1.0f, 1.0f, 1.0f, 1.0f - t));
  }
}

// static
void Impact::_bind_methods() {}

}  // namespace godot
