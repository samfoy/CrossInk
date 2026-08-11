#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cctype>
#include <climits>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "DictionaryWordGeometry.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchActionBar.h"
#include "components/UITheme.h"
#include "network/TranslateClient.h"
#include "network/TranslateCredentialStore.h"

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;

// A token is selectable when it has an ASCII alphanumeric or a non-ASCII
// codepoint outside U+2000-U+206F (dashes, bullets and other General
// Punctuation that appear as standalone tokens are not words).
bool isSelectableToken(const char* text) {
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p != 0; p++) {
    if (*p < 0x80) {
      if (std::isalnum(*p)) return true;
    } else if (*p == 0xE2 && (p[1] == 0x80 || p[1] == 0x81)) {
      if (p[2] == 0) break;  // truncated sequence: skipping would step past the NUL
      p += 2;                // skip the 3-byte General Punctuation codepoint
    } else {
      return true;
    }
  }
  return false;
}

void indexBuildYield(void*) { vTaskDelay(1); }

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  // No null check: a failed allocation just disables the differential
  // fast path (drawHighlightWithSnapshot skips the read), keeping the
  // full-repaint path as the fallback.
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  extractWords();
  // Long-press entry: start on the word under the finger. wordAt's slop is
  // finger-sized but still exact, so a press landing in a gutter or on
  // punctuation falls back to the nearest word on the touched line rather than
  // throwing the selection back to mid-page.
  bool positioned = false;
  if (!words.empty() && initialX >= 0 && initialY >= 0) {
    int hit = wordAt(initialX, initialY);
    if (hit < 0) hit = nearestWord(initialX, initialY);
    if (hit >= 0) {
      selected = hit;
      positioned = true;
    }
  }
  // Start on the middle row's word nearest mid-screen instead of top-left:
  // any word on the page is then at most half a page of moves away.
  if (!positioned && !words.empty()) {
    const int initial = closestInRow(rowCount / 2, renderer.getScreenWidth() / 2);
    if (initial >= 0) selected = initial;
  }
  requestUpdate();
}

void DictionaryWordSelectActivity::extractWords() {
  words.clear();
  words.reserve(128);
  rowCount = 0;

  // Single walk: collect the selectable words while accumulating their text
  // and styles (~2KB transient string, freed on return). Widths are measured
  // afterwards: merging the page's codepoints into the SD font's persistent
  // advance table first keeps getTextAdvanceX on the in-RAM path instead of
  // loading glyphs from SD one overflow slot at a time.
  std::string pageText;
  pageText.reserve(2048);
  uint8_t styleMask = 0;

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;

    bool rowHasWords = false;
    const int ascender = renderer.getFontAscenderSize(fontId);
    const int rubyShift = block->getRubyShift(ascender);
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const char* text = block->wordText(i);
      if (!isSelectableToken(text)) continue;

      WordBox box;
      box.x = static_cast<int16_t>(line->xPos + block->wordXpos(i) + marginLeft);
      box.y = static_cast<int16_t>(line->yPos + marginTop + rubyShift);
      box.style = block->wordStyle(i);
      box.width = 0;  // measured below, once the advance table is ready
      box.row = rowCount;
      box.text = text;
      words.push_back(box);
      rowHasWords = true;

      pageText.append(text);
      pageText.push_back(' ');
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(box.style) & 0x03));
    }
    if (rowHasWords) rowCount++;
  }

  if (styleMask == 0) styleMask = 0x01;  // REGULAR
  renderer.ensureSdCardFontReady(fontId, pageText.c_str(), styleMask);
  for (auto& word : words) {
    word.width = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, word.text, word.style));
  }
}

// Index of the word whose box (with finger-sized slop) contains the touch
// point; -1 when the touch lands on no word. Boxes never overlap after the
// slop grows them, at worst they touch, so first hit wins.
int DictionaryWordSelectActivity::wordAt(const int x, const int y) const {
  constexpr int SLOP = 4;  // matches the highlight box (+2) plus finger error
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    const WordBox& word = words[i];
    if (x >= word.x - SLOP && x < word.x + word.width + SLOP && y >= word.y - SLOP && y < word.y + lineHeight + SLOP) {
      return i;
    }
  }
  return -1;
}

// Index of the word closest to a touch point that hit no word's box, biased to
// the touched line: a press in the gutter, on punctuation or in the gap between
// two words resolves to the word the user was most plausibly aiming at. Rows
// within one line-height of the point are preferred (vertical distance wins),
// ties broken by horizontal distance to the word's box.
int DictionaryWordSelectActivity::nearestWord(const int x, const int y) const {
  // Geometry lives in DictionaryWordGeometry so the host-side gtest suite
  // exercises the same code that runs on device.
  return DictionaryWordGeometry::nearestWordIndex(x, y, static_cast<int>(words.size()), lineHeight, [this](int i) {
    const WordBox& word = words[i];
    return DictionaryWordGeometry::WordSpan{word.x, word.y, word.width};
  });
}

// Index of the word in `row` whose horizontal center is closest to centerX;
// -1 when the row has no words.
int DictionaryWordSelectActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

void DictionaryWordSelectActivity::moveVertical(const int direction) {
  const WordBox& current = words[selected];
  const int targetRow = static_cast<int>(current.row) + direction;
  if (targetRow < 0 || targetRow >= static_cast<int>(rowCount)) return;

  const int best = closestInRow(static_cast<uint16_t>(targetRow), current.x + current.width / 2);
  if (best >= 0 && best != selected) {
    selected = best;
    requestUpdate();
  }
}

// Translate the selected word via the marginalia bridge. Unlike performLookup
// this needs the network, so it can fail in ways a dictionary lookup cannot —
// each of which is named on screen rather than collapsing into a generic error.
// Reuses DictionaryDefinitionActivity for the result: it already scrolls, wraps
// and exits correctly, and a translation is the same shape as a definition.
//
// WiFi is NOT kept up while reading (it drains the battery and the reader turns
// it off), so a translate almost always starts disconnected. Rather than
// reporting "Wi-Fi not connected" and making the reader go turn it on by hand,
// this brings the radio up itself via WifiSelectionActivity, whose autoConnect
// re-joins the last used network without any prompt when credentials are saved.
void DictionaryWordSelectActivity::performTranslate() {
  if (words.empty()) return;

  if (!TRANSLATE_STORE.isConfigured()) {
    popup = Popup::Error;
    popupMsg = StrId::STR_TRANSLATE_NOT_CONFIGURED;
    popupTime = millis();
    requestUpdate();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Remember which word to translate: the sub-activity repaints over us, and
    // the selection must survive the round trip.
    pendingTranslateIndex = selected;
    wifiActivated = true;
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             const int index = pendingTranslateIndex;
                             pendingTranslateIndex = -1;
                             if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                               // The reader backed out of the picker, or the join
                               // failed: say so plainly instead of silently doing
                               // nothing.
                               popup = Popup::Error;
                               popupMsg = StrId::STR_TRANSLATE_NO_WIFI;
                               popupTime = millis();
                               requestUpdate();
                               return;
                             }
                             if (index >= 0 && index < static_cast<int>(words.size())) {
                               selected = index;
                               requestTranslateAfterWifi = true;
                             }
                             requestUpdate();
                           });
    return;
  }

  runTranslateRequest();
}

// The blocking half of performTranslate, split out so it can be reached both
// directly (WiFi already up) and from the WifiSelectionActivity result handler.
void DictionaryWordSelectActivity::runTranslateRequest() {
  if (words.empty() || selected < 0 || selected >= static_cast<int>(words.size())) return;

  popup = Popup::Busy;
  popupMsg = StrId::STR_TRANSLATING;
  requestUpdateAndWait();  // paint before blocking on the network

  const std::string word = words[selected].text;
  // Neighbouring words on the same line give the model enough to disambiguate a
  // single word ("banco" -> bench vs bank) without sending the whole page.
  const TranslateClient::Response resp = TranslateClient::translate(word, lineContext(selected));

  if (resp.result == TranslateClient::Result::Ok) {
    popup = Popup::None;
    std::string heading = word;
    startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(heading),
                                                                          std::string(resp.translation)),
                           [this](const ActivityResult&) { requestUpdate(); });
    return;
  }

  popup = Popup::Error;
  switch (resp.result) {
    case TranslateClient::Result::NoWifi:
      popupMsg = StrId::STR_TRANSLATE_NO_WIFI;
      break;
    case TranslateClient::Result::NotConfigured:
      popupMsg = StrId::STR_TRANSLATE_NOT_CONFIGURED;
      break;
    case TranslateClient::Result::Unauthorized:
      popupMsg = StrId::STR_TRANSLATE_UNAUTHORIZED;
      break;
    case TranslateClient::Result::TooLong:
      popupMsg = StrId::STR_TRANSLATE_TOO_LONG;
      break;
    default:
      popupMsg = StrId::STR_TRANSLATE_FAILED;
      break;
  }
  popupTime = millis();
  requestUpdate();
}

// Words on the same rendered line as `index`, joined — the surrounding sentence
// as far as this page's layout knows it. Capped so a wide line can't push the
// request over the bridge's selection limit.
std::string DictionaryWordSelectActivity::lineContext(const int index) const {
  if (index < 0 || index >= static_cast<int>(words.size())) return {};
  const uint16_t row = words[index].row;
  std::string out;
  for (const WordBox& word : words) {
    if (word.row != row) continue;
    if (!out.empty()) out += ' ';
    out += word.text;
    if (out.size() > 400) break;
  }
  return out;
}

void DictionaryWordSelectActivity::performLookup() {
  popup = Popup::Busy;
  if (!dictOpenAttempted) {
    dictOpenAttempted = true;
    dictOpenOk = dict.open(SETTINGS.dictionaryName);
    // needsIndex() opens and validates the .qidx sidecar, so ask it once per
    // open rather than once per word: the answer only changes when we build
    // the sidecar ourselves, which is handled below.
    dictNeedsIndex = dictOpenOk && dict.needsIndex();
  }
  popupMsg = dictNeedsIndex ? StrId::STR_DICT_INDEXING : StrId::STR_DICT_LOOKING_UP;
  requestUpdateAndWait();  // paint the page + busy popup before blocking on SD

  bool ok = dictOpenOk;
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  if (ok && dictNeedsIndex) {
    ok = dict.buildIndex(&indexBuildYield, nullptr, &indexResult);
    dictNeedsIndex = !ok;  // a successful build leaves the sidecar fresh; a failed one retries
  }

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  const bool found = ok && dict.lookup(words[selected].text, definition, headword, &result);

  if (found) {
    popup = Popup::None;
    startActivityForResult(
        std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(headword),
                                                       std::move(definition), dict.definitionsAreHtml()),
        [this](const ActivityResult&) { requestUpdate(); });
    return;
  }
  // Name the failure: a genuine miss is "Not found"; a word that WAS found but
  // couldn't be read is a real error — and we distinguish decompression from a
  // low-memory allocation from a generic read error.
  if (!ok) {
    popup = Popup::Error;
    // An index build allocates a scan buffer, so it fails the same way lookups
    // do on a fragmented heap — name that rather than a generic error.
    switch (indexResult) {
      case Dictionary::IndexResult::LowMemory:
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::IndexResult::ReadError:
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::IndexResult::Ok:
      default:
        popupMsg = StrId::STR_DICT_ERROR;  // dict.open() failed, not the index
        break;
    }
  } else {
    switch (result) {
      case Dictionary::LookupResult::Decompress:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_DECOMPRESS_ERROR;
        break;
      case Dictionary::LookupResult::LowMemory:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::LookupResult::ReadError:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::LookupResult::NotFound:
      default:
        popup = Popup::NotFound;
        popupMsg = StrId::STR_DICT_NOT_FOUND;
        break;
    }
  }
  popupTime = millis();
  requestUpdate();
}

void DictionaryWordSelectActivity::onExit() {
  Activity::onExit();
  // Leave the radio as we found it: this screen only raises WiFi for a
  // translate, and reading with it up costs battery for nothing.
  if (wifiActivated) {
    WiFi.disconnect(false);
    wifiActivated = false;
  }
}

void DictionaryWordSelectActivity::loop() {
  // Deferred translate after the WiFi picker returned connected. Run it from
  // loop(), never from the result handler: runTranslateRequest blocks on the
  // network and calls requestUpdateAndWait, which must not happen inside an
  // activity-result callback.
  if (requestTranslateAfterWifi) {
    requestTranslateAfterWifi = false;
    runTranslateRequest();
    return;
  }

  if (popup == Popup::NotFound || popup == Popup::Error) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      popup = Popup::None;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmPressSeen = true;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && confirmPressSeen && !words.empty()) {
    performLookup();
    return;
  }

  if (words.empty() && !TouchActionBar::available(mappedInput)) return;

  // Touch: the action bar owns taps inside its band, so it is checked first —
  // otherwise a tap on "Close" would also land on whatever word sits above it.
  int tx = 0;
  int ty = 0;
  if (TouchActionBar::available(mappedInput)) {
    const int action = TouchActionBar::tapped(renderer, mappedInput, barActions());
    if (action == 0) {
      finish();
      return;
    }
    if (action == 1) {
      performLookup();
      return;
    }
    if (action == 2) {
      performTranslate();
      return;
    }
  }

  if (words.empty()) return;

  // A touch-down moves the highlight to the touched word (differential
  // repaint), a tap on a word selects and looks it up in one go.
  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    // Ignore a press that starts on the action bar: the highlight must not jump
    // to the word above the button the reader is aiming at.
    if (TouchActionBar::contains(renderer, mappedInput, ty)) return;
    const int hit = wordAt(tx, ty);
    if (hit >= 0 && hit != selected) {
      selected = hit;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (TouchActionBar::contains(renderer, mappedInput, ty)) return;
    const int hit = wordAt(tx, ty);
    if (hit >= 0) {
      selected = hit;
      performLookup();
    }
    return;
  }

  const bool hasNextWord = selected + 1 < static_cast<int>(words.size());
  if (mappedInput.wasPressed(MappedInputManager::Button::ScreenLeft) && selected > 0) {
    selected--;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenRight) && hasNextWord) {
    selected++;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown)) {
    moveVertical(1);
  }
}

// Saves the pixels under words[selected]'s highlight box, then draws the
// highlight over them. Returns false when the pixels could not be saved
// (no buffer / oversize box) — the highlight is drawn regardless, but the
// next cursor move must do a full repaint.
bool DictionaryWordSelectActivity::drawHighlightWithSnapshot() {
  const WordBox& word = words[selected];
  int hx = word.x - 2;
  int hy = word.y - 2;
  int hw = word.width + 4;
  int hh = lineHeight + 4;
  // Clamp to the panel so save, draw and restore all use the same box.
  if (hx < 0) {
    hw += hx;
    hx = 0;
  }
  if (hy < 0) {
    hh += hy;
    hy = 0;
  }

  bool saved = false;
  if (snapshot && hw > 0 && hh > 0) {
    saved = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  }
  snapshotX = static_cast<int16_t>(hx);
  snapshotY = static_cast<int16_t>(hy);
  snapshotW = static_cast<int16_t>(hw);
  snapshotH = static_cast<int16_t>(hh);
  snapshotIdx = saved ? selected : -1;

  renderer.fillRect(hx, hy, hw, hh, true);
  renderer.drawText(fontId, word.x, word.y, word.text, false, word.style);
  return saved;
}

// Front-button bar (Back/Confirm/Left/Right). Drawn last on every repaint
// path, including the differential highlight-only path, so it always ends
// up as the top layer even when a highlighted word's box falls under a
// hint's screen area. No side-button hints: the full-bleed reader page has no
// spare gutter for them, so a hint box there would hide text.
// Actions for the touch action bar, in bar order. Kept in one place so draw and
// hit-testing can never disagree about which slot is which.
std::vector<TouchActionBar::Action> DictionaryWordSelectActivity::barActions() const {
  const bool haveWord = !words.empty();
  return {
      {tr(STR_CLOSE), true},
      {tr(STR_LOOKUP), haveWord},
      {tr(STR_TRANSLATE), haveWord},
  };
}

void DictionaryWordSelectActivity::drawHints() const {
  // Touch boards get real on-screen controls: GUI.drawButtonHints() draws
  // nothing when hasTouch(), and this board has neither a Back nor a Confirm
  // button, so without this the screen has no visible way out at all.
  if (TouchActionBar::available(mappedInput)) {
    TouchActionBar::draw(renderer, mappedInput, barActions());
    return;
  }
  // No selectable word on this page: Confirm and navigation are all no-ops
  // (guarded by words.empty() in loop()/performLookup), so only Back does
  // anything and only Back is hinted.
  if (words.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_LOOKUP), tr(STR_DIR_LEFT),
                                                       tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  // Differential fast path: only the highlight moved and the framebuffer
  // still holds a clean page (no popup or sub-activity since the last full
  // repaint). Restore the pixels under the old highlight, draw the new one,
  // and push — skipping the two-pass page render entirely.
  if (popup == Popup::None && snapshotIdx >= 0 && !words.empty() && selected != snapshotIdx) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    // The full path's PrewarmScope cleared the glyph cache on exit; batch-load
    // just the highlighted word's glyphs before drawing them white-on-black.
    renderer.getFontCacheManager()->prewarmCache(
        fontId, words[selected].text, static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[selected].style) & 0x03)));
    if (drawHighlightWithSnapshot()) {
      drawHints();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    // Snapshot failed (oversize box) — fall through to a full repaint.
  }

  renderer.clearScreen();

  // Same prewarm-scan-then-render pass the reader uses, so SD-card fonts hit
  // the in-RAM glyph cache during the real draw.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  page->render(renderer, fontId, marginLeft, marginTop);

  if (!words.empty()) {
    drawHighlightWithSnapshot();
  }

  drawHints();

  if (popup != Popup::None) {
    // The popup overdraws the page, so the snapshot no longer matches the
    // framebuffer — force the next render onto the full-repaint path.
    snapshotIdx = -1;
    // drawPopup overlays the framebuffer and refreshes the display itself.
    // I18N.get directly: tr() only accepts literal key names.
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
