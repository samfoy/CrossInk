#include <gtest/gtest.h>

#include <set>

#include "src/components/TouchActionBarGeometry.h"

using namespace TouchActionBarGeometry;

namespace {

// X4 Pro portrait panel.
constexpr int kWidth = 480;
constexpr int kScreenHeight = 800;
constexpr int kBarHeight = 44;

// --- the property that actually matters -------------------------------------

// Every pixel of the bar must map to the slot that was PAINTED there. If draw
// and hit-test disagree by even one pixel, a tap near a divider activates the
// neighbouring button — the kind of bug that reads as "the UI is flaky".
TEST(TouchActionBarGeometry, EveryPixelMapsToThePaintedSlot) {
  for (int count = 1; count <= 5; count++) {
    for (int x = 0; x < kWidth; x++) {
      const int slot = slotAt(kWidth, count, x);
      ASSERT_GE(slot, 0) << "unassigned pixel x=" << x << " count=" << count;
      ASSERT_LT(slot, count);
      EXPECT_GE(x, slotStart(kWidth, count, slot));
      EXPECT_LT(x, slotEnd(kWidth, count, slot));
    }
  }
}

// Slots must tile the bar with no gap and no overlap.
TEST(TouchActionBarGeometry, SlotsTileTheBarExactly) {
  for (int count = 1; count <= 5; count++) {
    EXPECT_EQ(slotStart(kWidth, count, 0), 0);
    EXPECT_EQ(slotEnd(kWidth, count, count - 1), kWidth);
    for (int i = 1; i < count; i++) {
      EXPECT_EQ(slotStart(kWidth, count, i), slotEnd(kWidth, count, i - 1))
          << "gap/overlap between slot " << (i - 1) << " and " << i;
    }
  }
}

// --- the 3-button bar the dictionary screen actually uses --------------------

TEST(TouchActionBarGeometry, ThreeSlotsOn480Panel) {
  EXPECT_EQ(slotAt(kWidth, 3, 0), 0);
  EXPECT_EQ(slotAt(kWidth, 3, 159), 0);
  EXPECT_EQ(slotAt(kWidth, 3, 160), 1);
  EXPECT_EQ(slotAt(kWidth, 3, 319), 1);
  EXPECT_EQ(slotAt(kWidth, 3, 320), 2);
  EXPECT_EQ(slotAt(kWidth, 3, 479), 2);
}

// A width that doesn't divide evenly must still tile: the rounding has to land
// somewhere, but never as a gap or an overlap.
TEST(TouchActionBarGeometry, IndivisibleWidthStillTiles) {
  constexpr int odd = 481;
  std::set<int> seen;
  for (int x = 0; x < odd; x++) {
    const int slot = slotAt(odd, 3, x);
    ASSERT_GE(slot, 0) << "unassigned pixel at x=" << x;
    seen.insert(slot);
  }
  EXPECT_EQ(seen.size(), 3u) << "every slot must be reachable";
  EXPECT_EQ(slotEnd(odd, 3, 2), odd);
}

// --- out of bounds ----------------------------------------------------------

TEST(TouchActionBarGeometry, OutsideTheBarIsNoSlot) {
  EXPECT_EQ(slotAt(kWidth, 3, -1), -1);
  EXPECT_EQ(slotAt(kWidth, 3, kWidth), -1);
  EXPECT_EQ(slotAt(kWidth, 3, kWidth + 100), -1);
}

TEST(TouchActionBarGeometry, DegenerateCountsAreRefused) {
  EXPECT_EQ(slotAt(kWidth, 0, 10), -1);
  EXPECT_EQ(slotAt(kWidth, -1, 10), -1);
  EXPECT_EQ(slotAt(0, 3, 0), -1);
}

// --- vertical band ----------------------------------------------------------

TEST(TouchActionBarGeometry, BarOccupiesTheBottomBand) {
  EXPECT_EQ(barTop(kScreenHeight, kBarHeight), 756);
  EXPECT_FALSE(inBar(kScreenHeight, kBarHeight, 755));
  EXPECT_TRUE(inBar(kScreenHeight, kBarHeight, 756));
  EXPECT_TRUE(inBar(kScreenHeight, kBarHeight, 799));
}

// A press on the page above the bar must NOT be swallowed by it — that is what
// keeps word selection working while the bar is on screen.
TEST(TouchActionBarGeometry, PageAreaIsNotInTheBar) {
  for (int y = 0; y < barTop(kScreenHeight, kBarHeight); y++) {
    ASSERT_FALSE(inBar(kScreenHeight, kBarHeight, y)) << "y=" << y << " wrongly claimed by the bar";
  }
}

}  // namespace
