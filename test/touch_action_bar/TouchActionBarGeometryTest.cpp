#include <gtest/gtest.h>

#include <cstdlib>
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

// X4 Pro bezel covers the bottom 3 panel rows in portrait (ViewableInsets.bottom).
constexpr int kBottomInset = 3;

TEST(TouchActionBarGeometry, BarOccupiesTheBottomBand) {
  EXPECT_EQ(barTop(kScreenHeight, kBarHeight), 756);
  EXPECT_FALSE(inBar(kScreenHeight, kBarHeight, 755));
  EXPECT_TRUE(inBar(kScreenHeight, kBarHeight, 756));
  EXPECT_TRUE(inBar(kScreenHeight, kBarHeight, 799));
}

// The bug Sam hit: with no inset the bar ran to the last panel row, so its lower
// edge (divider, label descenders) sat under the bezel and looked cut off. The
// bar must be LIFTED by the inset, not merely clipped.
TEST(TouchActionBarGeometry, BarIsLiftedClearOfTheBezel) {
  const int top = barTop(kScreenHeight, kBarHeight, kBottomInset);
  const int bottom = barBottom(kScreenHeight, kBottomInset);
  EXPECT_EQ(top, 753) << "bar must start higher to clear the bezel";
  EXPECT_EQ(bottom, 797) << "bar must end above the bezel rows";
  EXPECT_EQ(bottom - top, kBarHeight) << "full height preserved, not clipped";
  // The bezel rows themselves are outside the bar.
  for (int y = bottom; y < kScreenHeight; y++) {
    EXPECT_FALSE(inBar(kScreenHeight, kBarHeight, y, kBottomInset)) << "bezel row y=" << y << " must not be in the bar";
  }
  EXPECT_TRUE(inBar(kScreenHeight, kBarHeight, top, kBottomInset));
  EXPECT_TRUE(inBar(kScreenHeight, kBarHeight, bottom - 1, kBottomInset));
  EXPECT_FALSE(inBar(kScreenHeight, kBarHeight, top - 1, kBottomInset));
}

// Labels must sit inside the band with room for descenders at both ends, not
// pinned at a fixed offset from the top.
TEST(TouchActionBarGeometry, LabelBaselineIsCentredInTheBand) {
  const int top = barTop(kScreenHeight, kBarHeight, kBottomInset);
  const int bottom = barBottom(kScreenHeight, kBottomInset);
  for (int textHeight : {10, 14, 18, 22}) {
    const int baseline = labelBaseline(top, bottom - top, textHeight);
    EXPECT_GE(baseline - textHeight, top) << "glyph top escapes the bar (textHeight=" << textHeight << ")";
    EXPECT_LE(baseline, bottom) << "baseline past the bar's lower edge (textHeight=" << textHeight << ")";
    // Centred: space above the glyph should match space below, within rounding.
    const int above = (baseline - textHeight) - top;
    const int below = bottom - baseline;
    EXPECT_LE(std::abs(above - below), 1) << "not centred (textHeight=" << textHeight << ")";
  }
}

// A press on the page above the bar must NOT be swallowed by it — that is what
// keeps word selection working while the bar is on screen.
TEST(TouchActionBarGeometry, PageAreaIsNotInTheBar) {
  for (int y = 0; y < barTop(kScreenHeight, kBarHeight, kBottomInset); y++) {
    ASSERT_FALSE(inBar(kScreenHeight, kBarHeight, y, kBottomInset)) << "y=" << y << " wrongly claimed by the bar";
  }
}

}  // namespace
