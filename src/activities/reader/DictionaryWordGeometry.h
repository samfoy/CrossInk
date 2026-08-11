#pragma once

#include <climits>

// Pure geometry for dictionary word selection: no rendering, font or FreeRTOS
// dependencies, so it is exercised directly by the host-side gtest suite
// (test/dictionary_word_geometry).
namespace DictionaryWordGeometry {

// Screen box of one selectable word, reduced to the fields the geometry needs.
// Height is not stored per word: every word on a line shares the page's
// lineHeight, matching how DictionaryWordSelectActivity lays words out.
struct WordSpan {
  int x;
  int y;
  int width;
};

// Distance from `v` to the span [lo, hi), 0 when inside it. The +1 on the upper
// side keeps the first pixel outside the box a non-zero distance, so "just past
// the right edge" never ties with "exactly on the edge".
inline int distanceToRange(const int v, const int lo, const int hi) {
  if (v < lo) return lo - v;
  if (v >= hi) return v - hi + 1;
  return 0;
}

// Rank of a word box relative to a touch point: vertical distance to the word's
// line band dominates, horizontal distance to the box breaks ties. Rows are
// weighted by ROW_WEIGHT so a word on the touched line always beats a
// horizontally closer word on a different line — a press in the gap between two
// words must not jump to the line above or below.
constexpr long ROW_WEIGHT = 4096;

inline long score(const int x, const int y, const WordSpan& word, const int lineHeight) {
  const long dy = distanceToRange(y, word.y, word.y + lineHeight);
  const long dx = distanceToRange(x, word.x, word.x + word.width);
  return dy * ROW_WEIGHT + dx;
}

// Index of the word closest to a touch point that hit no word's box, biased to
// the touched line: a press in a gutter, on punctuation, or in the gap between
// two words resolves to the word the user was most plausibly aiming at.
// Returns -1 when there are no words. Ties keep the earliest word in reading
// order, so the result is deterministic.
template <typename BoxAt>
int nearestWordIndex(const int x, const int y, const int count, const int lineHeight, BoxAt boxAt) {
  int best = -1;
  long bestScore = LONG_MAX;
  for (int i = 0; i < count; i++) {
    const long s = score(x, y, boxAt(i), lineHeight);
    if (s < bestScore) {
      bestScore = s;
      best = i;
    }
  }
  return best;
}

}  // namespace DictionaryWordGeometry
