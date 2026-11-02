#pragma once
#include <cstdint>
namespace tongzhuo {
enum class Reveal { Hidden, Strip, Panel };
struct HoverSample {
  int x = 0, y = 0;
  bool valid = true, proximity = false, lip = false, inside = false,
       down = false, movingWindow = false, suppressed = false;
  uint64_t now = 0;
};
// Adopted behavior from ToolDock's HoverIntent: releasing a drag must not
// reveal anything until a fresh, button-free pointer movement occurs.
class HoverReveal {
  bool initialized = false, armed = false, wasDown = false, dismissed = false;
  int lastX = 0, lastY = 0;
  uint64_t approached = 0, entered = 0, left = 0;

public:
  Reveal state = Reveal::Hidden;
  void reset() {
    state = Reveal::Hidden;
    initialized = false;
    armed = false;
    wasDown = false;
    dismissed = false;
    approached = entered = left = 0;
  }
  // A selection or Esc stays closed until the pointer leaves the activation
  // zone.
  void dismiss() {
    reset();
    dismissed = true;
  }
  Reveal update(HoverSample s) {
    bool moved = initialized && (s.x != lastX || s.y != lastY);
    bool released = wasDown && !s.down;
    initialized = true;
    lastX = s.x;
    lastY = s.y;
    wasDown = s.down;
    if (!s.valid || s.suppressed) {
      reset();
      return state;
    }
    if (dismissed) {
      if (!s.proximity && !s.inside)
        dismissed = false;
      return state;
    }
    bool owns = state == Reveal::Panel && s.inside && !s.movingWindow;
    if (s.down || s.movingWindow) {
      if (!owns) {
        state = Reveal::Hidden;
        approached = entered = left = 0;
      }
      armed = false;
      return state;
    }
    if (moved && !released)
      armed = true;
    if (state == Reveal::Panel) {
      if (s.inside) {
        left = 0;
        return state;
      }
      if (!left)
        left = s.now;
      if (s.now - left >= 420) {
        state = Reveal::Hidden;
        approached = entered = left = 0;
        armed = false;
      }
      return state;
    }
    if (!armed || !s.proximity) {
      state = Reveal::Hidden;
      approached = entered = 0;
      return state;
    }
    if (!approached)
      approached = s.now;
    if (state == Reveal::Hidden && s.now - approached >= 90)
      state = Reveal::Strip;
    if (state == Reveal::Strip && s.lip) {
      if (!entered)
        entered = s.now;
      if (s.now - entered >= 140) {
        state = Reveal::Panel;
        left = 0;
      }
    } else
      entered = 0;
    return state;
  }
};
} // namespace tongzhuo
