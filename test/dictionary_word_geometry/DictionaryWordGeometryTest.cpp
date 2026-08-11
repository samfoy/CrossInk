#include <gtest/gtest.h>

#include <vector>

#include "src/activities/reader/DictionaryWordGeometry.h"

using DictionaryWordGeometry::nearestWordIndex;
using DictionaryWordGeometry::WordSpan;

namespace {

constexpr int kLineHeight = 20;

// Three-line page layout, words laid out left-to-right per line. Left margin 10,
// lines at y=0/20/40. A gap separates word 0 from word 1 on the top line.
//
//   idx  line  x range        y range
//   0    0     [10,  50)      [ 0, 20)   "alpha"
//   1    0     [90, 130)      [ 0, 20)   "beta"    (gap 50..90)
//   2    1     [10,  60)      [20, 40)   "gamma"
//   3    1     [70, 110)      [20, 40)   "delta"
//   4    2     [10,  70)      [40, 60)   "epsilon"
const std::vector<WordSpan>& layout() {
  static const std::vector<WordSpan> words = {
      {10, 0, 40}, {90, 0, 40}, {10, 20, 50}, {70, 20, 40}, {10, 40, 60},
  };
  return words;
}

int nearest(const int x, const int y, const std::vector<WordSpan>& words = layout()) {
  return nearestWordIndex(x, y, static_cast<int>(words.size()), kLineHeight,
                          [&words](const int i) { return words[i]; });
}

// --- degenerate input -------------------------------------------------------

TEST(DictionaryWordGeometry, EmptyPageReturnsNoWord) {
  const std::vector<WordSpan> none;
  EXPECT_EQ(nearest(20, 10, none), -1);
}

// --- direct hits ------------------------------------------------------------

TEST(DictionaryWordGeometry, PointInsideBoxPicksThatWord) {
  EXPECT_EQ(nearest(20, 10), 0);
  EXPECT_EQ(nearest(100, 10), 1);
  EXPECT_EQ(nearest(30, 30), 2);
  EXPECT_EQ(nearest(80, 30), 3);
  EXPECT_EQ(nearest(40, 50), 4);
}

// --- the line-bias property (the reason rows are weighted) -------------------

// A press in the left margin resolves to the first word of the line pressed,
// never to a word on a neighbouring line.
TEST(DictionaryWordGeometry, LeftMarginPicksFirstWordOfTouchedLine) {
  EXPECT_EQ(nearest(0, 10), 0);
  EXPECT_EQ(nearest(0, 30), 2);
  EXPECT_EQ(nearest(0, 50), 4);
}

// A press in the right margin resolves to the last word of the line pressed.
TEST(DictionaryWordGeometry, RightMarginPicksLastWordOfTouchedLine) {
  EXPECT_EQ(nearest(300, 10), 1);
  EXPECT_EQ(nearest(300, 30), 3);
  EXPECT_EQ(nearest(300, 50), 4);
}

// The decisive case: a point in the top line's word gap is horizontally much
// closer to a word one line below, but must stay on the line the user touched.
TEST(DictionaryWordGeometry, LineBiasBeatsHorizontalProximity) {
  // x=65 sits in the top-line gap [50,90). On line 1, word 3 starts at x=70 —
  // only 5px away horizontally — but it is on a different line.
  const int hit = nearest(65, 10);
  EXPECT_TRUE(hit == 0 || hit == 1) << "expected a top-line word, got index " << hit;
}

// --- gaps between words -----------------------------------------------------

TEST(DictionaryWordGeometry, GapResolvesToNearerNeighbourOnSameLine) {
  // Gap is [50, 90): x=55 is 6px past word 0's right edge, 35px before word 1.
  EXPECT_EQ(nearest(55, 10), 0);
  // x=85 is 36px past word 0, 5px before word 1.
  EXPECT_EQ(nearest(85, 10), 1);
}

TEST(DictionaryWordGeometry, GapMidpointIsDeterministic) {
  // Word 0 ends at 50, word 1 starts at 90. distanceToRange makes "just past
  // the edge" cost 1 more than "on the edge", so the exact tie sits at x=69/70.
  const int left = nearest(69, 10);
  const int right = nearest(70, 10);
  EXPECT_EQ(left, 0);
  EXPECT_EQ(right, 1);
  // Repeated calls agree (no dependence on iteration state).
  EXPECT_EQ(nearest(69, 10), left);
}

// --- presses above / below the text block -----------------------------------

TEST(DictionaryWordGeometry, PressAboveFirstLineFallsToFirstLine) {
  const int hit = nearest(20, -30);
  EXPECT_EQ(hit, 0);
}

TEST(DictionaryWordGeometry, PressBelowLastLineFallsToLastLine) {
  const int hit = nearest(20, 200);
  EXPECT_EQ(hit, 4);
}

// --- boundary arithmetic ----------------------------------------------------

TEST(DictionaryWordGeometry, DistanceToRangeEdges) {
  using DictionaryWordGeometry::distanceToRange;
  EXPECT_EQ(distanceToRange(10, 10, 50), 0) << "lower edge is inside";
  EXPECT_EQ(distanceToRange(49, 10, 50), 0) << "last pixel inside";
  EXPECT_EQ(distanceToRange(50, 10, 50), 1) << "first pixel outside costs 1";
  EXPECT_EQ(distanceToRange(9, 10, 50), 1);
  EXPECT_EQ(distanceToRange(0, 10, 50), 10);
  EXPECT_EQ(distanceToRange(60, 10, 50), 11);
}

// A word's own line band is inclusive of its top row and exclusive of the next
// line's top row, so adjacent lines never both score 0 vertically.
TEST(DictionaryWordGeometry, LineBandsDoNotOverlap) {
  using DictionaryWordGeometry::score;
  const WordSpan top{10, 0, 40};
  const WordSpan below{10, 20, 50};
  // y=19 is the last row of the top line.
  EXPECT_LT(score(20, 19, top, kLineHeight), score(20, 19, below, kLineHeight));
  // y=20 is the first row of the line below.
  EXPECT_LT(score(20, 20, below, kLineHeight), score(20, 20, top, kLineHeight));
}

}  // namespace
