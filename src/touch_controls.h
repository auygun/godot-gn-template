#ifndef SRC_TOUCH_CONTROLS_H_
#define SRC_TOUCH_CONTROLS_H_

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/vector2.hpp>

namespace godot {

class FpsCharacter;

// Mobile touch controls: floating joystick (left zone), look (right zone),
// and FIRE/JUMP/SPRINT/RELOAD buttons (dragging FIRE also aims).
// Auto-disables on devices without a touchscreen.
class TouchControls : public Control {
  GDCLASS(TouchControls, Control);

 public:
  TouchControls();
  ~TouchControls() override;

  void _ready() override;
  void _gui_input(const Ref<InputEvent>& event) override;
  void _draw() override;

  bool enabled() const { return enabled_; }
  void set_enabled(bool enabled);

 protected:
  static void _bind_methods();

  void _notification(int what);

 private:
  enum class TouchRole { kNone, kJoystick, kLook, kButton };

  struct Button {
    Vector2 center;
    float radius = 0.0f;
    StringName action;
    String label;
    bool look = false;  // dragging on the button also aims
    bool pressed = false;
  };

  struct TouchState {
    TouchRole role = TouchRole::kNone;
    Button* button = nullptr;
    Vector2 last_pos;
  };

  static constexpr int kMaxTouches = 16;
  static constexpr int kButtonCount = 4;
  static constexpr float kJoyZoneRatio = 0.45f;  // left part of the screen
  static constexpr float kJoyDeadzone = 0.1f;
  static constexpr float kTouchSensitivity = 0.003f;  // radians per pixel

  Button* FindButton(const Vector2& pos);
  void ReleaseAll();
  void SetAction(const StringName& action, float strength);
  void UpdateMoveActions();
  void Layout();
  void TouchBegan(int index, const Vector2& pos);
  void TouchMoved(int index, const Vector2& pos);
  void TouchEnded(int index);

  TouchState touches_[kMaxTouches] = {};
  Button buttons_[kButtonCount] = {};
  FpsCharacter* character_ = nullptr;
  bool enabled_ = true;
  float joy_radius_ = 90.0f;
  Vector2 joy_center_;
  Vector2 joy_vector_;
  bool joy_active_ = false;
};

}  // namespace godot

#endif  // SRC_TOUCH_CONTROLS_H_
