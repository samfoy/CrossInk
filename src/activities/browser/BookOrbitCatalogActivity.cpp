#include "BookOrbitCatalogActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr int SECTION_COUNT = 2;  // Continue Reading, All Books

std::string sectionTitle(int idx) {
  return idx == 0 ? std::string(tr(STR_CONTINUE_READING)) : std::string(tr(STR_ALL_BOOKS));
}

std::string catalogErr(KOReaderCatalogClient::Error e) {
  switch (e) {
    case KOReaderCatalogClient::UNAVAILABLE:
      return std::string(tr(STR_CATALOG_UNAVAILABLE));
    case KOReaderCatalogClient::NO_CREDENTIALS:
      return std::string(tr(STR_NO_CREDENTIALS_MSG));
    case KOReaderCatalogClient::PARSE_ERROR:
      return std::string(tr(STR_PARSE_FEED_FAILED));
    default:
      return std::string(tr(STR_FETCH_FEED_FAILED));  // network/transport/heap
  }
}
}  // namespace

void BookOrbitCatalogActivity::onEnter() {
  Activity::onEnter();
  // Free as much heap as possible before the TLS handshake — the C3's heap dips
  // to a few KB mid-handshake, so match the kosync activity and drop the SD font
  // registry too (not just the loaded font).
  sdFontSystem.releaseForNetwork(renderer);

  state = State::CHECK_WIFI;
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  items.clear();
  listPage = 1;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  checkAndConnectWifi();
}

void BookOrbitCatalogActivity::onExit() {
  Activity::onExit();
  items.clear();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

bool BookOrbitCatalogActivity::preventAutoSleep() {
  switch (state) {
    case State::CHECK_WIFI:
    case State::WIFI_SELECTION:
    case State::LOADING:
    case State::DOWNLOADING:
      return true;
    default:
      return false;
  }
}

void BookOrbitCatalogActivity::loop() {
  if (state == State::WIFI_SELECTION) return;

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  if (state == State::LOADING || state == State::CHECK_WIFI || state == State::DOWNLOADING) {
    // Downloads poll Back internally; loading screens allow Back to bail home.
    if (state != State::DOWNLOADING && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      enterSections();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::SECTIONS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      section = selectorIndex == 0 ? Section::CONTINUE_READING : Section::ALL_BOOKS;
      section == Section::CONTINUE_READING ? loadContinueReading() : loadAllBooks(1);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    } else {
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, SECTION_COUNT);
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, SECTION_COUNT);
        requestUpdate();
      });
    }
    return;
  }

  if (state == State::LIST) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!items.empty()) openBookDetail(items[selectorIndex].id);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      enterSections();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      // Prev page (all-books only)
      if (section == Section::ALL_BOOKS && listPage > 1) loadAllBooks(listPage - 1);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (section == Section::ALL_BOOKS && listHasNext) loadAllBooks(listPage + 1);
    } else if (!items.empty()) {
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, static_cast<int>(items.size()));
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, static_cast<int>(items.size()));
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, static_cast<int>(items.size()), PAGE_ITEMS);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, static_cast<int>(items.size()), PAGE_ITEMS);
        requestUpdate();
      });
    }
    return;
  }

  if (state == State::DETAIL) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Confirm = Download (then open); if already downloaded, Open.
      if (!downloadedPath.empty()) {
        activityManager.goToReader(downloadedPath);
      } else if (detail.primaryFileId > 0) {
        downloadCurrentBook();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      // Right = Mark Finished (writes read-status back to the server).
      markCurrentBookFinished();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      downloadedPath.clear();
      state = State::LIST;
      requestUpdate();
    }
    return;
  }
}

void BookOrbitCatalogActivity::enterSections() {
  selectorIndex = 0;
  items.clear();

  // Fetch dashboard stats once per visit (streak/goal/total + the Continue
  // Reading list). If we already have them (returning from a sub-list), just
  // show the sections screen without re-fetching.
  if (dashboardLoaded) {
    state = State::SECTIONS;
    requestUpdate();
    return;
  }

  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) requestUpdate(true);

  const auto e = KOReaderCatalogClient::fetchDashboard(dashboard, continueReadingCache);
  if (e != KOReaderCatalogClient::OK) {
    showError(catalogErr(e));
    return;
  }
  dashboardLoaded = true;
  state = State::SECTIONS;
  requestUpdate();
}

void BookOrbitCatalogActivity::loadContinueReading() {
  // Reuse the list already fetched with the dashboard — no second round-trip.
  items = continueReadingCache;
  selectorIndex = 0;
  section = Section::CONTINUE_READING;
  state = State::LIST;
  requestUpdate();
}

void BookOrbitCatalogActivity::loadAllBooks(int page) {
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) requestUpdate(true);

  BookOrbitCatalogPage pg;
  const auto e = KOReaderCatalogClient::fetchBooks("title", page, pg);
  if (e != KOReaderCatalogClient::OK) {
    showError(catalogErr(e));
    return;
  }
  items = std::move(pg.items);
  listPage = pg.page;
  listHasNext = pg.hasNext;
  listTotal = pg.total;
  selectorIndex = 0;
  state = State::LIST;
  requestUpdate();
}

void BookOrbitCatalogActivity::openBookDetail(int bookId) {
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) requestUpdate(true);

  downloadedPath.clear();
  const auto e = KOReaderCatalogClient::fetchDetail(bookId, detail);
  if (e != KOReaderCatalogClient::OK) {
    showError(catalogErr(e));
    return;
  }
  state = State::DETAIL;
  requestUpdate();
}

std::string BookOrbitCatalogActivity::destPathForDetail() const {
  std::string base = detail.author.empty() ? detail.title : detail.title + " - " + detail.author;
  // Save to root, matching the OPDS browser — downloaded books then appear at the
  // top level of the library like side-loaded ones (not hidden in a subfolder).
  return "/" + StringUtils::sanitizeFilename(base) + ".epub";
}

void BookOrbitCatalogActivity::downloadCurrentBook() {
  if (detail.primaryFileId <= 0) {
    showError(tr(STR_DOWNLOAD_FAILED));
    return;
  }
  state = State::DOWNLOADING;
  statusMessage = detail.title;
  downloadProgress = downloadTotal = 0;
  cancelRequested = false;
  // Render the "Downloading..." screen ONCE, synchronously, and wait for it to
  // finish BEFORE starting the transfer. Rendering runs on a separate task and
  // allocates framebuffer memory; if it runs concurrently with the download it
  // fragments the heap and HTTPClient's per-chunk malloc fails (error -8,
  // TOO_LESS_RAM, ~39 KB in). So we do zero renders during writeToStream.
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) requestUpdate(true);

  const std::string dest = destPathForDetail();

  // Download via the catalog client's insecure-TLS path (WiFiClientSecure +
  // setInsecure), the same stack kosync uses — the esp_http_client/HttpDownloader
  // path can't do insecure TLS and OOMs loading the CA bundle on the C3.
  const auto result = KOReaderCatalogClient::downloadFile(
      detail.primaryFileId, dest,
      [](size_t downloaded, size_t total, void* ctx) {
        auto* self = static_cast<BookOrbitCatalogActivity*>(ctx);
        self->downloadProgress = downloaded;
        self->downloadTotal = total;
        // Only poll for cancel here — deliberately NO requestUpdate(). Triggering
        // an e-ink render mid-transfer races HTTPClient's malloc and OOMs the C3.
        // Progress numbers are captured; the screen repaints once on completion.
        self->mappedInput.update();
        if (self->mappedInput.isPressed(MappedInputManager::Button::Back) ||
            self->mappedInput.wasReleased(MappedInputManager::Button::Back)) {
          self->cancelRequested = true;
        }
      },
      this, &cancelRequested, detail.primarySizeBytes);

  if (result == KOReaderCatalogClient::OK) {
    clearBookCache(dest);
    downloadedPath = dest;
    state = State::DETAIL;
  } else if (cancelRequested) {
    mappedInput.suppressNextBackRelease();
    state = State::DETAIL;
  } else {
    showError(tr(STR_DOWNLOAD_FAILED));
    return;
  }
  requestUpdate();
}

void BookOrbitCatalogActivity::markCurrentBookFinished() {
  if (detail.id <= 0) return;
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) requestUpdate(true);

  const auto e = KOReaderCatalogClient::setReadStatus(detail.id, "finished");
  if (e == KOReaderCatalogClient::OK) {
    detail.readStatus = "read";
  }
  state = State::DETAIL;
  requestUpdate();
}

void BookOrbitCatalogActivity::showError(const std::string& msg) {
  state = State::ERROR;
  errorMessage = msg;
  requestUpdate();
}

void BookOrbitCatalogActivity::checkAndConnectWifi() {
  if (!KOREADER_STORE.hasCredentials()) {
    showError(tr(STR_NO_CREDENTIALS_MSG));
    return;
  }
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    enterSections();
    return;
  }
  launchWifiSelection();
}

void BookOrbitCatalogActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookOrbitCatalogActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    enterSections();
  } else {
    showError(tr(STR_WIFI_CONN_FAILED));
  }
}

void BookOrbitCatalogActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_BOOKORBIT_LIBRARY), true, EpdFontFamily::BOLD);

  if (state == State::CHECK_WIFI || state == State::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
      char pctLine[16];
      char kbLine[32];
      const int pct = static_cast<int>((downloadProgress * 100) / downloadTotal);
      snprintf(pctLine, sizeof(pctLine), "%d%%", pct);
      snprintf(kbLine, sizeof(kbLine), "%u / %u KB", static_cast<unsigned>(downloadProgress / 1024),
               static_cast<unsigned>(downloadTotal / 1024));
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 55, pctLine);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 80, kbLine);
    } else if (downloadProgress > 0) {
      // Unknown total (chunked): show bytes received so the user sees motion.
      char bytesLine[32];
      snprintf(bytesLine, sizeof(bytesLine), "%u KB", static_cast<unsigned>(downloadProgress / 1024));
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, bytesLine);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::SECTIONS) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    // --- Dashboard stats header ---
    int y = 50;
    if (!dashboard.displayName.empty()) {
      auto who = renderer.truncatedText(UI_10_FONT_ID, dashboard.displayName.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, y, who.c_str());
      y += 26;
    }
    char line[64];
    if (dashboard.currentStreak >= 0) {
      snprintf(line, sizeof(line), "%s: %d %s", tr(STR_READING_STREAK), dashboard.currentStreak,
               dashboard.currentStreak == 1 ? tr(STR_DAY) : tr(STR_DAYS));
      renderer.drawText(UI_10_FONT_ID, 20, y, line);
      y += 24;
    }
    if (dashboard.goalBooks >= 0) {
      snprintf(line, sizeof(line), "%s: %d / %d", tr(STR_READING_GOAL), dashboard.goalCompleted,
               dashboard.goalBooks);
      renderer.drawText(UI_10_FONT_ID, 20, y, line);
      y += 24;
    }
    if (dashboard.totalBooks >= 0) {
      snprintf(line, sizeof(line), "%s: %d", tr(STR_TOTAL_BOOKS), dashboard.totalBooks);
      renderer.drawText(UI_10_FONT_ID, 20, y, line);
      y += 24;
    }

    // Divider + section rows below the stats.
    y += 6;
    renderer.drawLine(20, y, pageWidth - 20, y);
    y += 10;
    const int rowTop = y;
    constexpr int ROW_H = 30;
    renderer.fillRect(0, rowTop + selectorIndex * ROW_H - 2, pageWidth - 1, ROW_H);
    for (int i = 0; i < SECTION_COUNT; i++) {
      renderer.drawText(UI_10_FONT_ID, 20, rowTop + i * ROW_H, sectionTitle(i).c_str(), i != selectorIndex);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == State::DETAIL) {
    int y = 55;
    auto t = renderer.truncatedText(UI_12_FONT_ID, detail.title.c_str(), pageWidth - 40);
    renderer.drawText(UI_12_FONT_ID, 20, y, t.c_str(), true, EpdFontFamily::BOLD);
    y += 28;
    if (!detail.author.empty()) {
      auto a = renderer.truncatedText(UI_10_FONT_ID, detail.author.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, y, a.c_str());
      y += 24;
    }
    if (!detail.seriesName.empty()) {
      std::string s = detail.seriesName;
      if (detail.seriesIndex >= 0) s += " #" + std::to_string(detail.seriesIndex);
      auto sr = renderer.truncatedText(UI_10_FONT_ID, s.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, y, sr.c_str());
      y += 24;
    }
    // Status line: format + size + read status.
    std::string meta;
    if (!detail.primaryFormat.empty()) meta += detail.primaryFormat;
    if (detail.primarySizeBytes > 0) {
      char buf[32];
      snprintf(buf, sizeof(buf), " · %.1f MB", detail.primarySizeBytes / 1048576.0);
      meta += buf;
    }
    if (!detail.readStatus.empty()) meta += " · " + detail.readStatus;
    if (!meta.empty()) {
      renderer.drawText(UI_10_FONT_ID, 20, y, meta.c_str());
      y += 24;
    }
    if (!downloadedPath.empty()) {
      renderer.drawText(UI_10_FONT_ID, 20, y, tr(STR_DOWNLOAD_COMPLETE));
      y += 24;
    }

    const char* confirmLabel = downloadedPath.empty() ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_MARK_FINISHED), "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // State::LIST
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (items.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
    renderer.displayBuffer();
    return;
  }

  const int pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
  renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);
  for (int i = pageStartIndex; i < static_cast<int>(items.size()) && i < pageStartIndex + PAGE_ITEMS; i++) {
    const auto& it = items[i];
    std::string displayText = it.title;
    if (!it.author.empty()) displayText += " - " + it.author;
    if (it.progressPercentage >= 0) {
      char buf[16];
      snprintf(buf, sizeof(buf), "  (%d%%)", static_cast<int>(it.progressPercentage));
      displayText += buf;
    }
    auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
    renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(), i != selectorIndex);
  }
  renderer.displayBuffer();
}
