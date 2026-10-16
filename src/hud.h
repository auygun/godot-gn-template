#ifndef SRC_HUD_H_
#define SRC_HUD_H_

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

namespace godot {

class ColorRect;
class Label;

class Hud : public CanvasLayer {
  GDCLASS(Hud, CanvasLayer);

 public:
  Hud();
  ~Hud() override;

  void _ready() override;
  void _process(double delta) override;

  void UpdateAmmo(int ammo, int reserve, bool reloading);
  // The local player's health and this match's score; cheap to call every
  // tick (labels change only when the values do).
  void UpdateStatus(int health, bool dead, int kills, int deaths);
  void ShowHitmarker();
  // Flash the screen red: the local player was hit.
  void ShowDamage();

 protected:
  static void _bind_methods();

 private:
  static constexpr float kDamageFlashTime = 0.35f;  // seconds
  static constexpr float kDamageFlashAlpha = 0.35f;

  Label* ammo_label_ = nullptr;
  Label* score_label_ = nullptr;
  Label* health_label_ = nullptr;
  Label* dead_label_ = nullptr;
  ColorRect* damage_flash_ = nullptr;
  float damage_timer_ = 0.0f;
  Label* fps_label_ = nullptr;
  Label* hitmarker_label_ = nullptr;
  float hitmarker_timer_ = 0.0f;
  // What the labels show, to skip unchanged updates.
  int health_ = -1;
  bool dead_ = false;
  int kills_ = -1;
  int deaths_ = -1;
  int fps_ = -1;
};

}  // namespace godot

#endif  // SRC_HUD_H_
