#ifndef SRC_NET_VIEW_SMOOTHER_H_
#define SRC_NET_VIEW_SMOOTHER_H_

#include <cstdint>
#include <deque>

#include "src/net/replicate.h"

namespace godot::net {

// How the machine that commands an entity draws it (the server's own
// player, a client's predicted one): every frame, the state of its last
// two ticks drawn between them by each field's DrawFn, plus the
// corrections of its predictions, each eased out by the field's EaseFn.
//
// A correction is the difference, where the view was at the end of the
// last tick's interval, between what the prediction drew and what the
// corrected state draws: added to the corrected view, it continues where
// the predicted one was, and then eases out.
class ViewSmoother {
 public:
  ViewSmoother();
  ~ViewSmoother();

  // Forget the entity drawn.
  void Reset();
  // After the entity's tick (dt seconds): its state now.
  void Tick(const Box& state, float dt);
  // A client's prediction was corrected, before the next tick: the
  // corrected state of the last tick, and of the one before (null: as
  // predicted).
  void Corrected(const Box* prev, const Box& cur);
  // A correction moved a kTeleport field: draw cur from now on, without
  // easing.
  void Teleported(const Box& cur);

  bool has_state() const { return has_state_; }
  // The state to draw, fraction (0..1) of the way through the tick after
  // the last.
  void View(float fraction, Box* out) const;
  // The last tick's state with the corrections drawn at it (what a
  // physics tick sees, e.g. to aim from the view).
  void Latest(Box* out) const;

 private:
  // A correction is forgotten once every field has eased out, or this
  // long after it anyway.
  static constexpr float kMaxCorrectionAge = 2.0f;  // seconds

  struct Correction {
    // The eased fields' differences.
    Box diff;
    // ticks_ when it was made.
    uint32_t tick = 0;
  };

  // How long ago the correction was made, fraction of the way through the
  // tick after the last.
  float Age(const Correction& correction, float fraction) const;

  bool has_state_ = false;
  Box prev_;
  Box cur_;
  float dt_ = 0.0f;
  uint32_t ticks_ = 0;
  // Oldest first.
  std::deque<Correction> corrections_;
};

}  // namespace godot::net

#endif  // SRC_NET_VIEW_SMOOTHER_H_
