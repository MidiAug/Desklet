#pragma once
#include <algorithm>
#include <cstddef>

namespace tongzhuo {
constexpr int PreviewWidth = 256, PreviewHeight = 144;
constexpr size_t PreviewCacheLimit = 4, IconCacheLimit = 48;
constexpr int EdgeIdleInterval = 120, EdgeActiveInterval = 35;

// One panel, one visibility intent for the whole process. Desktop notifications
// only request pin verification; they must never change opened or activate it.
struct SharedPanelState {
  bool opened = false;
  unsigned pinAttempts = 0;
  void open() {
    opened = true;
    pinAttempts = 0;
  }
  void close() {
    opened = false;
    pinAttempts = 0;
  }
  bool mayRetryPin(bool pinned) const {
    return opened && !pinned && pinAttempts < 10;
  }
};
struct PopupBounds {
  int x, y, width, height;
};
inline float previewHeight(float width) { return width * 9.f / 16.f; }
inline int desktopCardHeight(int width, int availableHeight = 620) {
  int desired = 44 + (int)(previewHeight((float)width - 20) + .5f) + 18;
  return std::min(desired, std::max(100, availableHeight - 270));
}
struct PreviewBounds {
  float x, y, width, height;
};
inline PreviewBounds cardPreview(float width, float height) {
  float w = std::max(16.f, std::min(width - 20, (height - 62) * 16.f / 9.f));
  return {(width - w) / 2, 44, w, previewHeight(w)};
}
inline PopupBounds fitPanel(int left, int top, int width, int height,
                            int workLeft, int workTop, int workRight,
                            int workBottom) {
  width = std::min(width, std::max(1, workRight - workLeft));
  height = std::min(height, std::max(1, workBottom - workTop));
  return {std::clamp(left, workLeft, workRight - width),
          std::clamp(top, workTop, workBottom - height), width, height};
}
inline PopupBounds popupCentered(int ownerLeft, int ownerTop, int ownerRight,
                                 int ownerBottom, int width, int height,
                                 int workLeft, int workTop, int workRight,
                                 int workBottom) {
  width = std::min(width, std::max(1, workRight - workLeft));
  height = std::min(height, std::max(1, workBottom - workTop));
  return {std::clamp(ownerLeft + (ownerRight - ownerLeft - width) / 2, workLeft,
                     workRight - width),
          std::clamp(ownerTop + (ownerBottom - ownerTop - height) / 2, workTop,
                     workBottom - height),
          width, height};
}
inline PopupBounds popupAbove(int anchorX, int anchorY, int width, int height,
                              int left, int top, int right, int bottom,
                              int gap) {
  width = std::clamp(width, 1, std::max(1, right - left));
  height = std::clamp(height, 1, std::max(1, bottom - top));
  return {std::clamp(anchorX - width, left, right - width),
          std::clamp(anchorY - gap - height, top, bottom - height), width,
          height};
}
inline int hudLogicalWidth(int desktops, int available) {
  return std::min(available, (desktops + 1) * 158 + desktops * 8 + 20);
}
inline float hudTileWidth(float width, int desktops) {
  return (width - 20 - 8 * desktops) / (desktops + 1);
}

inline int lastDesktopPage(size_t count) {
  // The main overview includes a trailing "new desktop" tile.
  return static_cast<int>(count / 3);
}
inline int lastHudPage(size_t count) {
  return count ? static_cast<int>((count - 1) / 3) : 0;
}
inline int edgeInterval(bool proximity, bool shown) {
  return proximity || shown ? EdgeActiveInterval : EdgeIdleInterval;
}
inline bool needsLayout(bool first, int oldWidth, int oldHeight, int width,
                        int height, int oldPage, int page, bool oldSettings,
                        bool settings, size_t oldCount, size_t count) {
  return first || oldWidth != width || oldHeight != height || oldPage != page ||
         oldSettings != settings || oldCount != count;
}
} // namespace tongzhuo
