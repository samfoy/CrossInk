#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/TouchActionBarGeometry.h"
#include "fontIds.h"

/**
 * A bottom bar of tappable buttons for touch-only screens.
 *
 * GUI.drawButtonHints() early-returns on touch boards, which is right for boards
 * with physical buttons to label but leaves a touch-only screen with no visible
 * affordances at all. On the X4 Pro that is acute: Back and Confirm are both
 * PIN_UNASSIGNED, so the only way out of a screen is a left-edge swipe nobody
 * can discover. This gives those screens real on-screen controls.
 *
 * Draws nothing and reports no taps on boards without touch — they keep the
 * physical button hints, so a screen can safely use both.
 */
class TouchActionBar {
 public:
  struct Action {
    const char* label;
    bool enabled = true;
  };

  // Height reserved at the bottom of the screen. Callers must keep content out
  // of this band (the reader's page margins already do).
  static constexpr int HEIGHT = 44;

  static bool available(const MappedInputManager& input) { return input.hasTouch(); }

  // Panel rows the bezel physically covers at the bottom, in the current
  // orientation. The bar must sit ABOVE these or its lower edge — dividers,
  // descenders, the label itself — disappears under the bezel.
  static int bottomInset(const GfxRenderer& renderer) {
    int top = 0;
    int right = 0;
    int bottom = 0;
    int left = 0;
    renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
    return bottom;
  }

  // Reserved height for layout: 0 when there is no touch, so non-touch boards
  // keep their full page area. Includes the bezel inset the bar is lifted by, so
  // a caller reserving this much always clears the whole bar.
  static int reservedHeight(const GfxRenderer& renderer, const MappedInputManager& input) {
    return available(input) ? HEIGHT + bottomInset(renderer) : 0;
  }

  static void draw(const GfxRenderer& renderer, const MappedInputManager& input, const std::vector<Action>& actions) {
    if (!available(input) || actions.empty()) return;

    const int width = renderer.getScreenWidth();
    const int inset = bottomInset(renderer);
    const int top = TouchActionBarGeometry::barTop(renderer.getScreenHeight(), HEIGHT, inset);
    const int bottom = TouchActionBarGeometry::barBottom(renderer.getScreenHeight(), inset);
    const int count = static_cast<int>(actions.size());

    // Clear the band first: on the reader's differential repaint path the page
    // text underneath is still in the framebuffer.
    renderer.fillRect(0, top, width, bottom - top, false);
    renderer.drawLine(0, top, width, top);

    // Centre labels vertically in the band rather than at a fixed offset, so a
    // taller UI font can't push descenders past the bar's lower edge.
    const int textHeight = renderer.getFontAscenderSize(UI_10_FONT_ID);
    const int baseline = TouchActionBarGeometry::labelBaseline(top, bottom - top, textHeight);

    for (int i = 0; i < count; i++) {
      const int x = TouchActionBarGeometry::slotStart(width, count, i);
      const int nextX = TouchActionBarGeometry::slotEnd(width, count, i);
      if (i > 0) renderer.drawLine(x, top, x, bottom);
      const char* label = actions[i].label;
      if (label == nullptr || label[0] == '\0') continue;
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
      const int textX = x + ((nextX - x) - textWidth) / 2;
      // A disabled action is drawn so the bar's layout never shifts between
      // renders (which would make the buttons move under the reader's finger),
      // but it is visually distinct and refuses taps.
      renderer.drawText(UI_10_FONT_ID, textX, baseline, label, actions[i].enabled);
    }
  }

  /**
   * Index of the action the reader tapped, or -1.
   *
   * Consumes the tap only when it lands inside the bar, so a tap on the page
   * above still reaches the screen's own handler.
   */
  static int tapped(const GfxRenderer& renderer, const MappedInputManager& input, const std::vector<Action>& actions) {
    if (!available(input) || actions.empty()) return -1;
    int x = 0;
    int y = 0;
    if (!input.wasScreenTapped(x, y)) return -1;
    const int inset = bottomInset(renderer);
    if (!TouchActionBarGeometry::inBar(renderer.getScreenHeight(), HEIGHT, y, inset)) return -1;
    const int slot = TouchActionBarGeometry::slotAt(renderer.getScreenWidth(), static_cast<int>(actions.size()), x);
    if (slot < 0) return -1;
    return actions[slot].enabled ? slot : -1;
  }

  // True when the point falls inside the bar. Screens use this to keep a
  // touch-down (which moves the word highlight) from reacting to a press on the
  // bar itself. Uses >= barTop (not the exclusive band) on purpose: a press in
  // the dead bezel rows below the bar must also be ignored, never routed to a
  // word.
  static bool contains(const GfxRenderer& renderer, const MappedInputManager& input, int y) {
    return available(input) &&
           y >= TouchActionBarGeometry::barTop(renderer.getScreenHeight(), HEIGHT, bottomInset(renderer));
  }
};
