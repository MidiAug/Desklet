#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tongzhuo::compat {
using DesktopId = std::wstring;
using WindowId = uintptr_t;
enum class Show { Hidden, Normal, Minimized, Maximized };
struct Intent {
  Show show = Show::Hidden;
  int x = 100, y = 100, width = 640, height = 400;
};
// The adapter deliberately has NO "switch desktop" operation. Activations can
// be staged and rejected, but must never repair a jump by jumping back.
struct Windows {
  virtual ~Windows() = default;
  virtual bool valid(WindowId) = 0;
  virtual DesktopId desktop(WindowId) = 0;
  virtual DesktopId activeDesktop() = 0;
  virtual Intent capture(WindowId) = 0;
  virtual bool stageHidden(WindowId, const DesktopId &) = 0;
  virtual bool apply(WindowId, const Intent &, bool focus) = 0;
  virtual bool transfer(WindowId window, const DesktopId &target,
                        const Intent &intent) {
    return stageHidden(window, target) && apply(window, intent, false);
  }
  virtual bool group(WindowId, const std::wstring &) = 0;
  virtual WindowId createMulti(const DesktopId &) = 0;
};
struct MultiWindow {
  WindowId window = 0;
  DesktopId home;
};
class Core {
  Windows &windows;
  bool writing = false;
  bool recovering = false;
  std::map<DesktopId, Intent> singleIntent;
  std::vector<MultiWindow> multiple;
  WindowId single = 0;
  DesktopId current;
  const std::wstring multiGroup;

public:
  explicit Core(Windows &adapter, std::wstring group)
      : windows(adapter), multiGroup(std::move(group)) {}
  const DesktopId &workspace() const { return current; }
  bool busy() const { return writing; }
  const auto &multiWindows() const { return multiple; }
  const auto &savedIntents() const { return singleIntent; }
  Intent intent(const DesktopId &id) const {
    auto at = singleIntent.find(id);
    return at == singleIntent.end() ? Intent{} : at->second;
  }
  bool bindSingle(WindowId window, const DesktopId &home) {
    if (!windows.valid(window) || windows.desktop(window) != home)
      return false;
    single = window;
    current = home;
    singleIntent[home] = windows.capture(window);
    return true;
  }
  bool attachMulti(WindowId window, const DesktopId &home) {
    if (!windows.valid(window) || windows.desktop(window) != home)
      return false;
    for (auto &item : multiple)
      if (item.window == window)
        return false;
    multiple.push_back({window, home});
    return regroup();
  }
  bool regroup() {
    bool result = true;
    for (auto &item : multiple)
      if (windows.valid(item.window))
        result = windows.group(item.window,
                               item.home == current
                                   ? multiGroup
                                   : multiGroup + L".Parked." + item.home) &&
                 result;
    return result;
  }
  bool enter(const DesktopId &next) {
    if (next.empty() || writing)
      return false;
    if (next == current)
      return true;
    if (single && windows.valid(single)) {
      const Intent leaving = !recovering && windows.desktop(single) == current
                                 ? windows.capture(single)
                                 : intent(current);
      // Hold the source intent even if native hide/move fails. No target intent
      // may overwrite it, and current only commits after successful staging.
      writing = true;
      singleIntent[current] = leaving;
      bool staged = windows.transfer(single, next, intent(next));
      if (!staged) {
        recovering = true;
        writing = false;
        return false;
      }
      writing = false;
      recovering = false;
    }
    current = next;
    ++generation;
    return regroup();
  }
  bool activateSingle() {
    const auto here = windows.activeDesktop();
    if (here.empty() || !single || !windows.valid(single))
      return false;
    if (here != current && !enter(here))
      return false;
    // Validate the native home BEFORE any focus call.
    if (windows.desktop(single) != here) {
      writing = true;
      bool staged = windows.stageHidden(single, here);
      writing = false;
      if (!staged || windows.desktop(single) != here)
        return false;
    }
    Intent target = intent(here);
    if (target.show == Show::Hidden || target.show == Show::Minimized)
      target.show = Show::Normal;
    writing = true;
    bool shown = windows.apply(single, target, true);
    writing = false;
    if (shown)
      singleIntent[here] = target;
    return shown;
  }
  bool closeSingle() {
    if (!single || !windows.valid(single) || windows.desktop(single) != current)
      return false;
    Intent target = windows.capture(single);
    target.show = Show::Hidden;
    writing = true;
    bool hidden = windows.apply(single, target, false);
    writing = false;
    if (hidden)
      singleIntent[current] = target;
    return hidden;
  }
  // External observations are accepted only for this generation and workspace.
  // Controller-generated hide/restore events are never user intent.
  bool observeSingle(const DesktopId &source, uint64_t epoch, bool synthetic,
                     Intent value) {
    if (writing || synthetic || epoch != generation || source != current)
      return false;
    singleIntent[source] = value;
    return true;
  }
  WindowId activateMulti(bool forceNew = false) {
    auto here = windows.activeDesktop();
    if (here.empty() || (here != current && !enter(here)))
      return 0;
    if (!forceNew)
      for (auto &item : multiple)
        if (item.home == here && windows.valid(item.window) &&
            windows.desktop(item.window) == here) {
          auto placement = windows.capture(item.window);
          placement.show = Show::Normal;
          return windows.apply(item.window, placement, true) ? item.window : 0;
        }
    auto made = windows.createMulti(here);
    if (!made || !windows.valid(made) || windows.desktop(made) != here)
      return 0;
    if (!attachMulti(made, here))
      return 0;
    auto placement = windows.capture(made);
    placement.show = Show::Normal;
    return windows.apply(made, placement, true) ? made : 0;
  }
  uint64_t generation = 0;
};
} // namespace tongzhuo::compat
