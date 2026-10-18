#include "src/lag_compensation.h"

#include <algorithm>
#include <cmath>

#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/capsule_shape3d.hpp>
#include <godot_cpp/classes/collision_object3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/node3d.hpp>

namespace godot {

namespace {

// The distance along the ray (origin, unit direction) to where it enters
// the sphere, or a negative value if it misses. A ray starting inside hits
// at 0.
float RaySphere(const Vector3& origin,
                const Vector3& direction,
                const Vector3& center,
                float radius) {
  Vector3 m = origin - center;
  float b = m.dot(direction);
  float c = m.length_squared() - radius * radius;
  if (c <= 0.0f) {
    return 0.0f;
  }
  if (b > 0.0f) {
    return -1.0f;
  }
  float discriminant = b * b - c;
  if (discriminant < 0.0f) {
    return -1.0f;
  }
  return -b - std::sqrt(discriminant);
}

// The same for a capsule: a segment a..b swept by radius.
float RayCapsule(const Vector3& origin,
                 const Vector3& direction,
                 const Vector3& a,
                 const Vector3& b,
                 float radius,
                 Vector3* normal) {
  // Nearest approach between the ray and the segment's line, then test the
  // sphere at the segment's nearest point; refine once from the hit, which
  // is exact for the cylinder part.
  Vector3 axis = b - a;
  float axis_length_squared = axis.length_squared();
  auto closest_on_segment = [&](const Vector3& p) {
    if (axis_length_squared <= 0.0f) {
      return a;
    }
    float t = std::clamp((p - a).dot(axis) / axis_length_squared, 0.0f, 1.0f);
    return a + axis * t;
  };

  // Ray vs infinite cylinder.
  float best = -1.0f;
  if (axis_length_squared > 0.0f) {
    Vector3 axis_unit = axis / std::sqrt(axis_length_squared);
    Vector3 m = origin - a;
    Vector3 d_perp = direction - axis_unit * direction.dot(axis_unit);
    Vector3 m_perp = m - axis_unit * m.dot(axis_unit);
    float qa = d_perp.length_squared();
    float qb = m_perp.dot(d_perp);
    float qc = m_perp.length_squared() - radius * radius;
    if (qa > 1e-8f) {
      float discriminant = qb * qb - qa * qc;
      if (discriminant >= 0.0f) {
        float t = (-qb - std::sqrt(discriminant)) / qa;
        if (qc <= 0.0f) {
          t = 0.0f;  // Starts inside the cylinder.
        }
        if (t >= 0.0f) {
          float along = (origin + direction * t - a).dot(axis_unit);
          if (along >= 0.0f && along * along <= axis_length_squared) {
            best = t;
          }
        }
      }
    }
  }
  // The end caps (and a ray along the axis).
  for (const Vector3& center : {a, b}) {
    float t = RaySphere(origin, direction, center, radius);
    if (t >= 0.0f && (best < 0.0f || t < best)) {
      best = t;
    }
  }
  if (best >= 0.0f && normal) {
    Vector3 hit = origin + direction * best;
    *normal = (hit - closest_on_segment(hit)).normalized();
  }
  return best;
}

// The same for a box (half extents) in its local space (an unscaled
// transform, so distances are the same there).
float RayBox(const Vector3& origin,
             const Vector3& direction,
             const Vector3& half_extents,
             Vector3* normal) {
  float t_near = -1e30f;
  float t_far = 1e30f;
  int near_axis = 0;
  float near_sign = 1.0f;
  for (int axis = 0; axis < 3; axis++) {
    float o = origin[axis];
    float d = direction[axis];
    float h = half_extents[axis];
    if (std::abs(d) < 1e-8f) {
      if (o < -h || o > h) {
        return -1.0f;
      }
      continue;
    }
    float t1 = (-h - o) / d;
    float t2 = (h - o) / d;
    float sign = -1.0f;
    if (t1 > t2) {
      std::swap(t1, t2);
      sign = 1.0f;
    }
    if (t1 > t_near) {
      t_near = t1;
      near_axis = axis;
      near_sign = sign;
    }
    t_far = std::min(t_far, t2);
    if (t_near > t_far) {
      return -1.0f;
    }
  }
  if (t_far < 0.0f) {
    return -1.0f;
  }
  if (normal) {
    *normal = Vector3();
    (*normal)[near_axis] = near_sign;
  }
  return std::max(t_near, 0.0f);
}

}  // namespace

LagCompensation::LagCompensation(uint32_t shot_mask) : shot_mask_(shot_mask) {}

LagCompensation::~LagCompensation() = default;

bool LagCompensation::Hittable(const Node3D* body) const {
  const CollisionObject3D* object = Object::cast_to<CollisionObject3D>(body);
  if (!object || !(object->get_collision_layer() & shot_mask_)) {
    return false;
  }
  const CollisionShape3D* shape = Object::cast_to<CollisionShape3D>(
      body->get_node_or_null("CollisionShape3D"));
  return shape && !shape->is_disabled() && shape->get_shape().is_valid();
}

void LagCompensation::Record(uint32_t tick,
                             const std::vector<Node3D*>& bodies) {
  Frame& frame = frames_[tick % kHistoryTicks];
  frame.tick = tick;
  frame.valid = true;
  frame.hitboxes.clear();
  for (Node3D* body : bodies) {
    if (!Hittable(body)) {
      continue;
    }
    CollisionShape3D* shape = Object::cast_to<CollisionShape3D>(
        body->get_node_or_null("CollisionShape3D"));
    Hitbox hitbox;
    hitbox.node = body;
    hitbox.transform = shape->get_global_transform();
    Ref<CapsuleShape3D> capsule = shape->get_shape();
    Ref<BoxShape3D> box = shape->get_shape();
    if (capsule.is_valid()) {
      hitbox.capsule = true;
      hitbox.radius = capsule->get_radius();
      hitbox.height = capsule->get_height();
    } else if (box.is_valid()) {
      hitbox.half_extents = box->get_size() * 0.5f;
    } else {
      continue;
    }
    frame.hitboxes.push_back(hitbox);
  }
  newest_tick_ = tick;
}

uint32_t LagCompensation::oldest_tick() const {
  uint32_t oldest = newest_tick_;
  while (newest_tick_ - oldest + 1 < kHistoryTicks && FindFrame(oldest - 1)) {
    oldest--;
  }
  return oldest;
}

const LagCompensation::Frame* LagCompensation::FindFrame(uint32_t tick) const {
  const Frame& frame = frames_[tick % kHistoryTicks];
  return frame.valid && frame.tick == tick ? &frame : nullptr;
}

Node3D* LagCompensation::Raycast(float view_tick,
                                 const Vector3& origin,
                                 const Vector3& direction,
                                 float max_distance,
                                 const Node3D* ignore,
                                 Vector3* hit_position,
                                 Vector3* hit_normal) const {
  // The recorded ticks around view_tick (relative to the newest, as the
  // ticks wrap), clamped to what's kept.
  float back = static_cast<float>(newest_tick_) - view_tick;
  uint32_t oldest = oldest_tick();
  float max_back = static_cast<float>(newest_tick_ - oldest);
  back = std::clamp(back, 0.0f, max_back);
  uint32_t older_tick = newest_tick_ - static_cast<uint32_t>(std::ceil(back));
  float t = std::ceil(back) - back;  // towards the newer tick
  const Frame* older = FindFrame(older_tick);
  const Frame* newer = FindFrame(older_tick + 1);
  if (!older) {
    return nullptr;
  }
  if (!newer || t <= 0.0f) {
    newer = older;
    t = 0.0f;
  }

  Node3D* best_node = nullptr;
  float best_distance = max_distance;
  Vector3 best_normal;
  for (const Hitbox& hitbox : older->hitboxes) {
    if (hitbox.node == ignore) {
      continue;
    }
    // Blend towards the same body on the newer tick (absent there: it died
    // or left, so take it as it was).
    Transform3D xf = hitbox.transform;
    for (const Hitbox& next : newer->hitboxes) {
      if (next.node == hitbox.node) {
        xf.origin = xf.origin.lerp(next.transform.origin, t);
        break;
      }
    }

    float distance = -1.0f;
    Vector3 normal;
    if (hitbox.capsule) {
      // Godot's capsule height includes the end caps, along local Y.
      Vector3 up = xf.basis.get_column(1).normalized();
      float half = std::max(hitbox.height * 0.5f - hitbox.radius, 0.0f);
      distance = RayCapsule(origin, direction, xf.origin - up * half,
                            xf.origin + up * half, hitbox.radius, &normal);
    } else {
      Transform3D inverse = xf.affine_inverse();
      Vector3 local_origin = inverse.xform(origin);
      Vector3 local_direction = inverse.basis.xform(direction);
      distance =
          RayBox(local_origin, local_direction, hitbox.half_extents, &normal);
      normal = xf.basis.xform(normal).normalized();
    }
    if (distance >= 0.0f && distance < best_distance) {
      best_distance = distance;
      best_node = hitbox.node;
      best_normal = normal;
    }
  }
  if (best_node) {
    if (hit_position) {
      *hit_position = origin + direction * best_distance;
    }
    if (hit_normal) {
      *hit_normal = best_normal;
    }
  }
  return best_node;
}

}  // namespace godot
