#ifndef SRC_HUD_H_
#define SRC_HUD_H_

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

namespace godot {

class Label;

class Hud : public CanvasLayer {
  GDCLASS(Hud, CanvasLayer);

 public:
  Hud();
  ~Hud() override;

  void _ready() override;
  void _process(double delta) override;

  void UpdateAmmo(int ammo, int reserve, bool reloading);
  void AddScore(int points);
  void ShowHitmarker();

 protected:
  static void _bind_methods();

 private:
  Label* ammo_label_ = nullptr;
  Label* score_label_ = nullptr;
  Label* fps_label_ = nullptr;
  Label* hitmarker_label_ = nullptr;
  float hitmarker_timer_ = 0.0f;
  int score_ = 0;
  int fps_ = -1;
};

}  // namespace godot

#endif  // SRC_HUD_H_
