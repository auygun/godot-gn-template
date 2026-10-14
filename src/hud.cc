#include "src/hud.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/variant.hpp>
#ifdef DEBUG_ENABLED
#include <godot_cpp/variant/color.hpp>
#endif

namespace godot {

Hud::Hud() = default;

Hud::~Hud() = default;

void Hud::_ready() {
  ammo_label_ = Object::cast_to<Label>(get_node_or_null("Ammo"));
  score_label_ = Object::cast_to<Label>(get_node_or_null("Score"));
  fps_label_ = Object::cast_to<Label>(get_node_or_null("Fps"));
  hitmarker_label_ = Object::cast_to<Label>(get_node_or_null("Hitmarker"));

#ifdef DEBUG_ENABLED
  // Debug builds only: mark the build type in the top-right corner.
  Label* debug_label = memnew(Label);
  debug_label->set_name("Debug");
  debug_label->set_text("debug");
  debug_label->add_theme_font_size_override("font_size", 22);
  debug_label->add_theme_color_override("font_color", Color(1, 0.8f, 0.2f, 1));
  debug_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
  add_child(debug_label);
  debug_label->set_anchors_and_offsets_preset(Control::PRESET_TOP_RIGHT,
                                              Control::PRESET_MODE_MINSIZE, 20);
#endif
}

void Hud::_process(double delta) {
  float dt = static_cast<float>(delta);

  if (fps_label_) {
    // The engine averages this over a second, so it only changes about once a
    // second; skip the text update (and relayout) when it hasn't.
    int fps =
        static_cast<int>(Engine::get_singleton()->get_frames_per_second());
    if (fps != fps_) {
      fps_ = fps;
      fps_label_->set_text(vformat("FPS: %d", fps_));
    }
  }

  if (hitmarker_label_ && hitmarker_label_->is_visible()) {
    hitmarker_timer_ -= dt;
    if (hitmarker_timer_ <= 0.0f) {
      hitmarker_label_->set_visible(false);
    }
  }
}

void Hud::UpdateAmmo(int ammo, int reserve, bool reloading) {
  if (ammo_label_) {
    ammo_label_->set_text(reloading ? "RELOADING..."
                                    : vformat("%d / %d", ammo, reserve));
  }
}

void Hud::AddScore(int points) {
  score_ += points;
  if (score_label_) {
    score_label_->set_text(vformat("Score: %d", score_));
  }
}

void Hud::ShowHitmarker() {
  if (hitmarker_label_) {
    hitmarker_label_->set_visible(true);
    hitmarker_timer_ = 0.12f;  // seconds the hitmarker stays up
  }
}

// static
void Hud::_bind_methods() {
  ClassDB::bind_method(D_METHOD("update_ammo", "ammo", "reserve", "reloading"),
                       &Hud::UpdateAmmo);
  ClassDB::bind_method(D_METHOD("add_score", "points"), &Hud::AddScore);
  ClassDB::bind_method(D_METHOD("show_hitmarker"), &Hud::ShowHitmarker);
}

}  // namespace godot
