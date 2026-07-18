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
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include <HalStorage.h>
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;

// Labels for the browse menu rows (order matches BrowseMode enum).
std::string browseModeTitle(int idx) {
  switch (idx) {
    case 0:
      return std::string(tr(STR_CONTINUE_READING));
    case 1:
      return std::string(tr(STR_ALL_BOOKS));            // sort=title
    case 2:
      return std::string(tr(STR_RECENTLY_ADDED));
    case 3:
      return std::string(tr(STR_CURRENTLY_READING));
    case 4:
      return std::string(tr(STR_UNREAD));
    case 5:
      return std::string(tr(STR_FINISHED));
    default:
      return std::string(tr(STR_SEARCH));
  }
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
      selectBrowseMode(static_cast<BrowseMode>(selectorIndex));
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    } else {
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, BROWSE_MODE_COUNT);
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, BROWSE_MODE_COUNT);
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
      // Prev page (paged queries only; Continue Reading is a single cached list)
      if (browseMode != BrowseMode::CONTINUE_READING && listPage > 1) loadQuery(listQuery, listPage - 1);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (browseMode != BrowseMode::CONTINUE_READING && listHasNext) loadQuery(listQuery, listPage + 1);
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
      // Confirm = Open if the file is on the device, else Download (then open).
      if (!downloadedPath.empty()) {
        activityManager.goToReader(downloadedPath);
      } else if (alreadyOnDevice) {
        activityManager.goToReader(destPathForDetail());
      } else if (detail.primaryFileId > 0) {
        downloadCurrentBook();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      // Right = Mark Finished (writes read-status back to the server).
      markCurrentBookFinished();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      // Up = Download next in series (when one exists and isn't already here).
      if (detail.nextInSeriesId > 0) downloadNextInSeries();
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

void BookOrbitCatalogActivity::selectBrowseMode(BrowseMode mode) {
  browseMode = mode;
  switch (mode) {
    case BrowseMode::CONTINUE_READING:
      loadContinueReading();
      return;
    case BrowseMode::SEARCH:
      launchSearch();
      return;
    case BrowseMode::ALL_TITLE: {
      BookOrbitBooksQuery q;
      q.sort = "title";
      loadQuery(q, 1);
      return;
    }
    case BrowseMode::ALL_RECENT: {
      BookOrbitBooksQuery q;
      q.sort = "recently_added";
      loadQuery(q, 1);
      return;
    }
    case BrowseMode::CURRENTLY_READING: {
      BookOrbitBooksQuery q;
      q.readStatus = "reading";
      loadQuery(q, 1);
      return;
    }
    case BrowseMode::UNREAD: {
      BookOrbitBooksQuery q;
      q.readStatus = "unread";
      q.sort = "title";
      loadQuery(q, 1);
      return;
    }
    case BrowseMode::FINISHED: {
      BookOrbitBooksQuery q;
      q.readStatus = "finished";
      q.sort = "title";
      loadQuery(q, 1);
      return;
    }
  }
}

void BookOrbitCatalogActivity::loadContinueReading() {
  // Reuse the list already fetched with the dashboard — no second round-trip.
  items = continueReadingCache;
  selectorIndex = 0;
  browseMode = BrowseMode::CONTINUE_READING;
  state = State::LIST;
  requestUpdate();
}

void BookOrbitCatalogActivity::loadQuery(const BookOrbitBooksQuery& query, int page) {
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) requestUpdate(true);

  BookOrbitBooksQuery q = query;
  q.page = page;
  BookOrbitCatalogPage pg;
  const auto e = KOReaderCatalogClient::fetchBooks(q, pg);
  if (e != KOReaderCatalogClient::OK) {
    showError(catalogErr(e));
    return;
  }
  listQuery = q;
  items = std::move(pg.items);
  listPage = pg.page;
  listHasNext = pg.hasNext;
  listTotal = pg.total;
  selectorIndex = 0;
  state = State::LIST;
  requestUpdate();
}

void BookOrbitCatalogActivity::launchSearch() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH), searchTerm, 64, InputType::Text),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          state = State::SECTIONS;
          requestUpdate();
          return;
        }
        const auto& kb = std::get<KeyboardResult>(result.data);
        searchTerm = kb.text;
        if (searchTerm.empty()) {
          state = State::SECTIONS;
          requestUpdate();
          return;
        }
        browseMode = BrowseMode::SEARCH;
        BookOrbitBooksQuery q;
        q.search = searchTerm;
        loadQuery(q, 1);
      });
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
  refreshDownloadedFlag();
  state = State::DETAIL;
  requestUpdate();
}

void BookOrbitCatalogActivity::refreshDownloadedFlag() {
  // Show a ✓ / "Open" when this book's file already exists on the SD card.
  alreadyOnDevice = detail.primaryFileId > 0 && Storage.exists(destPathForDetail().c_str());
}

std::string BookOrbitCatalogActivity::destPathFor(const std::string& title, const std::string& author) {
  std::string base = author.empty() ? title : title + " - " + author;
  // Save to root, matching the OPDS browser — downloaded books then appear at the
  // top level of the library like side-loaded ones (not hidden in a subfolder).
  return "/" + StringUtils::sanitizeFilename(base) + ".epub";
}

std::string BookOrbitCatalogActivity::destPathForDetail() const { return destPathFor(detail.title, detail.author); }

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
    alreadyOnDevice = true;
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

void BookOrbitCatalogActivity::downloadNextInSeries() {
  if (detail.nextInSeriesId <= 0) return;
  // Load the next book's detail, then reuse the normal download flow.
  const int nextId = detail.nextInSeriesId;
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) requestUpdate(true);

  downloadedPath.clear();
  const auto e = KOReaderCatalogClient::fetchDetail(nextId, detail);
  if (e != KOReaderCatalogClient::OK) {
    showError(catalogErr(e));
    return;
  }
  refreshDownloadedFlag();
  if (alreadyOnDevice) {
    // Already have it — just show its detail so the user can open it.
    state = State::DETAIL;
    requestUpdate();
    return;
  }
  downloadCurrentBook();
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
    int y = 46;
    if (!dashboard.displayName.empty()) {
      auto who = renderer.truncatedText(UI_10_FONT_ID, dashboard.displayName.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, y, who.c_str());
      y += 24;
    }
    char line[64];
    if (dashboard.currentStreak >= 0) {
      snprintf(line, sizeof(line), "%s: %d %s", tr(STR_READING_STREAK), dashboard.currentStreak,
               dashboard.currentStreak == 1 ? tr(STR_DAY) : tr(STR_DAYS));
      renderer.drawText(UI_10_FONT_ID, 20, y, line);
      y += 22;
    }
    if (dashboard.goalBooks >= 0) {
      snprintf(line, sizeof(line), "%s: %d / %d", tr(STR_READING_GOAL), dashboard.goalCompleted, dashboard.goalBooks);
      renderer.drawText(UI_10_FONT_ID, 20, y, line);
      y += 22;
    }
    if (dashboard.totalBooks >= 0) {
      snprintf(line, sizeof(line), "%s: %d", tr(STR_TOTAL_BOOKS), dashboard.totalBooks);
      renderer.drawText(UI_10_FONT_ID, 20, y, line);
      y += 22;
    }

    // Divider + browse-mode rows below the stats.
    y += 4;
    renderer.drawLine(20, y, pageWidth - 20, y);
    y += 8;
    const int rowTop = y;
    constexpr int ROW_H = 26;
    // Keep the selected row visible by paging the menu if it grows past the screen.
    const int visibleRows = (pageHeight - rowTop - 20) / ROW_H;
    const int firstRow = (visibleRows > 0 && selectorIndex >= visibleRows) ? (selectorIndex - visibleRows + 1) : 0;
    for (int i = firstRow; i < BROWSE_MODE_COUNT && (i - firstRow) < visibleRows; i++) {
      const int rowY = rowTop + (i - firstRow) * ROW_H;
      if (i == selectorIndex) renderer.fillRect(0, rowY - 2, pageWidth - 1, ROW_H);
      renderer.drawText(UI_10_FONT_ID, 20, rowY, browseModeTitle(i).c_str(), i != selectorIndex);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == State::DETAIL) {
    const int textW = pageWidth - 40;
    int y = 50;
    auto t = renderer.truncatedText(UI_12_FONT_ID, detail.title.c_str(), textW);
    renderer.drawText(UI_12_FONT_ID, 20, y, t.c_str(), true, EpdFontFamily::BOLD);
    y += 26;
    if (!detail.author.empty()) {
      auto a = renderer.truncatedText(UI_10_FONT_ID, detail.author.c_str(), textW);
      renderer.drawText(UI_10_FONT_ID, 20, y, a.c_str());
      y += 22;
    }
    if (!detail.seriesName.empty()) {
      std::string s = detail.seriesName;
      if (detail.seriesIndex >= 0) s += " #" + std::to_string(detail.seriesIndex);
      auto sr = renderer.truncatedText(UI_10_FONT_ID, s.c_str(), textW);
      renderer.drawText(UI_10_FONT_ID, 20, y, sr.c_str());
      y += 22;
    }
    // Status line: format + size + read status.
    std::string meta;
    if (!detail.primaryFormat.empty()) meta += detail.primaryFormat;
    if (detail.primarySizeBytes > 0) {
      char buf[32];
      snprintf(buf, sizeof(buf), " %s %.1f MB", "\xC2\xB7", detail.primarySizeBytes / 1048576.0);
      meta += buf;
    }
    if (!detail.readStatus.empty()) meta += " \xC2\xB7 " + detail.readStatus;
    if (!meta.empty()) {
      renderer.drawText(UI_10_FONT_ID, 20, y, meta.c_str());
      y += 22;
    }
    // On-device / downloaded indicator.
    if (!downloadedPath.empty()) {
      renderer.drawText(UI_10_FONT_ID, 20, y, tr(STR_DOWNLOAD_COMPLETE));
      y += 22;
    } else if (alreadyOnDevice) {
      std::string chk = std::string("\xE2\x9C\x93 ") + tr(STR_ON_DEVICE);  // "✓ On device"
      renderer.drawText(UI_10_FONT_ID, 20, y, chk.c_str());
      y += 22;
    }
    // Next-in-series hint.
    if (detail.nextInSeriesId > 0 && !detail.nextInSeriesTitle.empty()) {
      std::string nx = std::string(tr(STR_NEXT_IN_SERIES)) + ": ";
      auto nxt = renderer.truncatedText(UI_10_FONT_ID, (nx + detail.nextInSeriesTitle).c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, y, nxt.c_str());
      y += 22;
    }

    const char* confirmLabel = (!downloadedPath.empty() || alreadyOnDevice) ? tr(STR_OPEN) : tr(STR_DOWNLOAD);
    const char* upLabel = detail.nextInSeriesId > 0 ? tr(STR_NEXT_IN_SERIES) : "";
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_MARK_FINISHED), upLabel);
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
    // ✓ prefix when this book already exists on the device.
    std::string displayText;
    if (Storage.exists(destPathFor(it.title, it.author).c_str())) displayText += "\xE2\x9C\x93 ";
    displayText += it.title;
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
