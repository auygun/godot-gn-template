#include "src/touch_controls.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_screen_drag.hpp>
#include <godot_cpp/classes/input_event_screen_touch.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "src/fps_character.h"

namespace godot {

TouchControls::TouchControls() = default;

TouchControls::~TouchControls() = default;

void TouchControls::_ready() {
  set_mouse_filter(MOUSE_FILTER_PASS);
  character_ = Object::cast_to<FpsCharacter>(get_parent());
  if (!DisplayServer::get_singleton()->is_touchscreen_available()) {
    enabled_ = false;
  }
  Layout();
  queue_redraw();
}

void TouchControls::_gui_input(const Ref<InputEvent>& event) {
  if (!enabled_) {
    return;
  }

  if (event->is_class("InputEventScreenTouch")) {
    const Ref<InputEventScreenTouch> touch = event;
    int index = touch->get_index();
    if (index < 0 || index >= kMaxTouches) {
      return;
    }
    if (touch->is_pressed()) {
      TouchBegan(index, touch->get_position());
    } else {
      TouchEnded(index);
    }
  } else if (event->is_class("InputEventScreenDrag")) {
    const Ref<InputEventScreenDrag> drag = event;
    int index = drag->get_index();
    if (index < 0 || index >= kMaxTouches) {
      return;
    }
    if (touches_[index].role != TouchRole::kNone) {
      TouchMoved(index, drag->get_position());
    }
  }
}

void TouchControls::_draw() {
  if (!enabled_) {
    return;
  }

  Ref<Font> font = ThemeDB::get_singleton()->get_fallback_font();
  Size2 size = get_size();

  // Joystick.
  if (joy_active_) {
    draw_circle(joy_center_, joy_radius_, Color(1.0f, 1.0f, 1.0f, 0.12f));
    draw_circle(joy_center_, joy_radius_, Color(1.0f, 1.0f, 1.0f, 0.35f), false,
                2.0f);
    draw_circle(joy_center_ + joy_vector_ * joy_radius_, joy_radius_ * 0.45f,
                Color(1.0f, 1.0f, 1.0f, 0.4f));
  } else {
    Vector2 hint = Vector2(size.width * 0.15f, size.height * 0.6f);
    draw_circle(hint, joy_radius_, Color(1.0f, 1.0f, 1.0f, 0.06f));
    draw_circle(hint, joy_radius_, Color(1.0f, 1.0f, 1.0f, 0.15f), false, 2.0f);
  }

  // Buttons.
  for (int i = 0; i < kButtonCount; i++) {
    const Button& b = buttons_[i];
    if (b.radius <= 0.0f) {
      continue;
    }
    Color fill = b.pressed ? Color(1.0f, 1.0f, 1.0f, 0.35f)
                           : Color(1.0f, 1.0f, 1.0f, 0.12f);
    draw_circle(b.center, b.radius, fill);
    draw_circle(b.center, b.radius, Color(1.0f, 1.0f, 1.0f, 0.4f), false, 2.0f);
    int font_size = static_cast<int>(b.radius * 0.4f);
    draw_string(font, b.center - Vector2(b.radius, -font_size * 0.35f), b.label,
                HORIZONTAL_ALIGNMENT_CENTER, b.radius * 2.0f, font_size,
                Color(1.0f, 1.0f, 1.0f, 0.8f));
  }
}

void TouchControls::set_enabled(bool enabled) {
  if (enabled_ == enabled) {
    return;
  }
  enabled_ = enabled;
  if (!enabled_) {
    ReleaseAll();
  }
  queue_redraw();
}

// static
void TouchControls::_bind_methods() {
  ClassDB::bind_method(D_METHOD("set_enabled", "enabled"),
                       &TouchControls::set_enabled);
  ClassDB::bind_method(D_METHOD("is_enabled"), &TouchControls::enabled);

  ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled",
               "is_enabled");
}

void TouchControls::_notification(int what) {
  if (what == NOTIFICATION_RESIZED) {
    // The anchors keep us the size of the viewport, so this also covers
    // window resizes and screen rotation.
    Layout();
    queue_redraw();
  } else if (what == NOTIFICATION_APPLICATION_FOCUS_OUT) {
    // Leaving the app mid-touch never delivers the touch release, so let go
    // of everything we hold down.
    ReleaseAll();
  }
}

TouchControls::Button* TouchControls::FindButton(const Vector2& pos) {
  for (int i = 0; i < kButtonCount; i++) {
    if ((pos - buttons_[i].center).length() <= buttons_[i].radius) {
      return &buttons_[i];
    }
  }
  return nullptr;
}

void TouchControls::ReleaseAll() {
  Input* input = Input::get_singleton();
  joy_active_ = false;
  joy_vector_ = Vector2();
  for (int i = 0; i < kMaxTouches; i++) {
    touches_[i].role = TouchRole::kNone;
    touches_[i].button = nullptr;
  }
  for (int i = 0; i < kButtonCount; i++) {
    if (buttons_[i].pressed) {
      buttons_[i].pressed = false;
      input->action_release(buttons_[i].action);
    }
  }
  UpdateMoveActions();
  queue_redraw();
}

void TouchControls::SetAction(const StringName& action, float strength) {
  Input* input = Input::get_singleton();
  if (strength > 0.0f) {
    input->action_press(action, strength);
  } else {
    input->action_release(action);
  }
}

void TouchControls::UpdateMoveActions() {
  Vector2 v = joy_vector_;
  if (v.length() < kJoyDeadzone) {
    v = Vector2();
  }
  SetAction("move_forward", v.y < 0.0f ? -v.y : 0.0f);
  SetAction("move_back", v.y > 0.0f ? v.y : 0.0f);
  SetAction("move_left", v.x < 0.0f ? -v.x : 0.0f);
  SetAction("move_right", v.x > 0.0f ? v.x : 0.0f);
}

void TouchControls::Layout() {
  Size2 size = get_size();
  if (size.width <= 0.0f || size.height <= 0.0f) {
    return;
  }
  float s = size.height / 1000.0f;
  float margin = 70.0f * s;
  float gap = 150.0f * s;
  buttons_[0] = Button{Vector2(size.width - margin, size.height - margin),
                       62.0f * s, "jump", "JUMP"};
  buttons_[1] = Button{Vector2(size.width - margin - gap, size.height - margin),
                       52.0f * s, "sprint", "SPRINT"};
  buttons_[2] = Button{Vector2(size.width - margin, size.height - margin - gap),
                       52.0f * s, "reload", "RELOAD"};
  buttons_[3] = Button{Vector2(size.width - margin - gap * 1.3f,
                               size.height - margin - gap * 1.4f),
                       80.0f * s, "shoot", "FIRE", true};
  buttons_[4] =
      Button{Vector2(size.width - margin, size.height - margin - gap * 2.0f),
             52.0f * s, "spawn_crate", "CRATE"};
  joy_radius_ = 90.0f * s;
}

void TouchControls::TouchBegan(int index, const Vector2& pos) {
  TouchState& state = touches_[index];
  state.last_pos = pos;

  Button* button = FindButton(pos);
  if (button) {
    state.role = TouchRole::kButton;
    state.button = button;
    button->pressed = true;
    Input::get_singleton()->action_press(button->action);
    queue_redraw();
    return;
  }

  if (!joy_active_ && pos.x < get_size().x * kJoyZoneRatio) {
    state.role = TouchRole::kJoystick;
    joy_active_ = true;
    joy_center_ = pos;
    joy_vector_ = Vector2();
    UpdateMoveActions();
    queue_redraw();
  } else {
    state.role = TouchRole::kLook;
  }
}

void TouchControls::TouchMoved(int index, const Vector2& pos) {
  TouchState& state = touches_[index];
  Vector2 delta = pos - state.last_pos;
  state.last_pos = pos;

  if (state.role == TouchRole::kJoystick) {
    Vector2 offset = pos - joy_center_;
    float len = offset.length();
    if (len > joy_radius_) {
      offset = offset / len * joy_radius_;
    }
    joy_vector_ = offset / joy_radius_;
    UpdateMoveActions();
    queue_redraw();
  } else if (state.role == TouchRole::kLook ||
             (state.role == TouchRole::kButton && state.button->look)) {
    if (character_) {
      character_->ApplyLookDelta(delta, kTouchSensitivity);
    }
  }
}

void TouchControls::TouchEnded(int index) {
  TouchState& state = touches_[index];

  switch (state.role) {
    case TouchRole::kJoystick:
      joy_active_ = false;
      joy_vector_ = Vector2();
      UpdateMoveActions();
      break;
    case TouchRole::kButton:
      if (state.button) {
        state.button->pressed = false;
        Input::get_singleton()->action_release(state.button->action);
      }
      break;
    case TouchRole::kLook:
    case TouchRole::kNone:
      break;
  }

  state.role = TouchRole::kNone;
  state.button = nullptr;
  queue_redraw();
}

}  // namespace godot
