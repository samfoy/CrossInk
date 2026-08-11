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

// What the bar must clear at the bottom: the bezel's viewable inset (3 on the
// X4 Pro, which uses the SDK default) PLUS the reader's status/progress bar band
// PLUS drawStatusBar's own 4px breathing room. Lifting by the 3px inset alone
// left the bar sitting on the status bar and still touching the panel edge.
constexpr int kBezelInset = 3;
constexpr int kClearance = 4;
constexpr int kStatusBarOff = kBezelInset + 8 + kClearance;      // screenMargin floor
constexpr int kProgressBarOnly = kBezelInset + 16 + kClearance;  // progress bar band
constexpr int kFullStatusBar = kBezelInset + 40 + kClearance;    // text lane + progress bar

TEST(TouchActionBarGeometry, BarOccupiesTheBottomBand) {
  EXPECT_EQ(barTop(kScreenHeight, kBarHeight), 756);
  EXPECT_FALSE(inBar(kScreenHeight, kBarHeight, 755));
  EXPECT_TRUE(inBar(kScreenHeight, kBarHeight, 756));
  EXPECT_TRUE(inBar(kScreenHeight, kBarHeight, 799));
}

// The bug Sam reported twice: the bar overlapped the bottom of the screen. The
// first fix lifted it by the bezel inset only, which is 3px on this board — not
// enough, because the reader's status/progress bar band lives there too. The bar
// must clear the WHOLE reserve and keep its full height.
TEST(TouchActionBarGeometry, BarClearsTheWholeBottomReserve) {
  for (int reserve : {kStatusBarOff, kProgressBarOnly, kFullStatusBar}) {
    const int top = barTop(kScreenHeight, kBarHeight, reserve);
    const int bottom = barBottom(kScreenHeight, reserve);
    EXPECT_EQ(bottom - top, kBarHeight) << "full height must survive (reserve=" << reserve << ")";
    EXPECT_EQ(kScreenHeight - bottom, reserve) << "gap below the bar must equal the reserve";
    // Every reserved row is outside the bar.
    for (int y = bottom; y < kScreenHeight; y++) {
      ASSERT_FALSE(inBar(kScreenHeight, kBarHeight, y, reserve))
          << "reserved row y=" << y << " claimed by the bar (reserve=" << reserve << ")";
    }
    EXPECT_TRUE(inBar(kScreenHeight, kBarHeight, top, reserve));
    EXPECT_TRUE(inBar(kScreenHeight, kBarHeight, bottom - 1, reserve));
  }
}

// Regression guard with real numbers: a 3px lift is NOT enough clearance. If a
// future change reverts to insets-only this fails loudly.
TEST(TouchActionBarGeometry, BezelInsetAloneIsInsufficientClearance) {
  const int insetOnly = kScreenHeight - barBottom(kScreenHeight, kBezelInset);
  EXPECT_EQ(insetOnly, 3);
  const int withBand = kScreenHeight - barBottom(kScreenHeight, kProgressBarOnly);
  EXPECT_GE(withBand, 15) << "must clear the status bar band, not just the bezel";
  EXPECT_GT(withBand, insetOnly);
}

// Labels must sit inside the band with room for descenders at both ends, not
// pinned at a fixed offset from the top.
TEST(TouchActionBarGeometry, LabelBaselineIsCentredInTheBand) {
  const int top = barTop(kScreenHeight, kBarHeight, kProgressBarOnly);
  const int bottom = barBottom(kScreenHeight, kProgressBarOnly);
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
  for (int y = 0; y < barTop(kScreenHeight, kBarHeight, kProgressBarOnly); y++) {
    ASSERT_FALSE(inBar(kScreenHeight, kBarHeight, y, kProgressBarOnly)) << "y=" << y << " wrongly claimed by the bar";
  }
}

}  // namespace
