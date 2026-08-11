#pragma once

// Slot geometry for a bottom action bar, split out so the host gtest suite can
// exercise the same arithmetic the device uses. Draw and hit-test MUST derive
// their bounds from these helpers, or a tap can land on a different button than
// the one that was painted.
namespace TouchActionBarGeometry {

// Left edge (inclusive) of slot `index` of `count`, across `width` pixels.
inline int slotStart(const int width, const int count, const int index) { return (width * index) / count; }

// Right edge (exclusive) of slot `index`.
inline int slotEnd(const int width, const int count, const int index) { return (width * (index + 1)) / count; }

// Slot containing `x`, or -1 when outside the bar. Uses the same half-open
// [start, end) bounds as slotStart/slotEnd so adjacent slots can never both
// claim a pixel and no pixel between 0 and width-1 is unassigned.
inline int slotAt(const int width, const int count, const int x) {
  if (count <= 0 || width <= 0) return -1;
  if (x < 0 || x >= width) return -1;
  for (int i = 0; i < count; i++) {
    if (x >= slotStart(width, count, i) && x < slotEnd(width, count, i)) return i;
  }
  return -1;
}

// Top edge of the bar for a screen of `screenHeight` pixels.
inline int barTop(const int screenHeight, const int barHeight) { return screenHeight - barHeight; }

// True when `y` falls inside the bar band.
inline bool inBar(const int screenHeight, const int barHeight, const int y) {
  return y >= barTop(screenHeight, barHeight);
}

}  // namespace TouchActionBarGeometry
