#include "src/hud.h"

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

Hud::Hud() = default;

Hud::~Hud() = default;

void Hud::_ready() {
  ammo_label_ = Object::cast_to<Label>(get_node_or_null("Ammo"));
  score_label_ = Object::cast_to<Label>(get_node_or_null("Score"));
  fps_label_ = Object::cast_to<Label>(get_node_or_null("Fps"));
  hitmarker_label_ = Object::cast_to<Label>(get_node_or_null("Hitmarker"));
  health_label_ = Object::cast_to<Label>(get_node_or_null("Health"));
  dead_label_ = Object::cast_to<Label>(get_node_or_null("Dead"));
  damage_flash_ = Object::cast_to<ColorRect>(get_node_or_null("DamageFlash"));

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

  if (damage_flash_ && damage_flash_->is_visible()) {
    damage_timer_ -= dt;
    if (damage_timer_ <= 0.0f) {
      damage_flash_->set_visible(false);
    } else {
      damage_flash_->set_color(
          Color(1, 0, 0, kDamageFlashAlpha * damage_timer_ / kDamageFlashTime));
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

void Hud::UpdateStatus(int health, bool dead, int kills, int deaths) {
  if (health != health_ && health_label_) {
    health_label_->set_text(vformat("Health: %d", health));
  }
  health_ = health;
  if (dead != dead_ && dead_label_) {
    dead_label_->set_visible(dead);
  }
  dead_ = dead;
  if ((kills != kills_ || deaths != deaths_) && score_label_) {
    score_label_->set_text(vformat("Kills: %d  Deaths: %d", kills, deaths));
  }
  kills_ = kills;
  deaths_ = deaths;
}

void Hud::ShowHitmarker() {
  if (hitmarker_label_) {
    hitmarker_label_->set_visible(true);
    hitmarker_timer_ = 0.12f;  // seconds the hitmarker stays up
  }
}

void Hud::ShowDamage() {
  if (damage_flash_) {
    damage_flash_->set_color(Color(1, 0, 0, kDamageFlashAlpha));
    damage_flash_->set_visible(true);
    damage_timer_ = kDamageFlashTime;
  }
}

// static
void Hud::_bind_methods() {
  ClassDB::bind_method(D_METHOD("update_ammo", "ammo", "reserve", "reloading"),
                       &Hud::UpdateAmmo);
  ClassDB::bind_method(
      D_METHOD("update_status", "health", "dead", "kills", "deaths"),
      &Hud::UpdateStatus);
  ClassDB::bind_method(D_METHOD("show_damage"), &Hud::ShowDamage);
  ClassDB::bind_method(D_METHOD("show_hitmarker"), &Hud::ShowHitmarker);
}

}  // namespace godot
