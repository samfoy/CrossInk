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
constexpr size_t CATALOG_DOWNLOAD_BUFFER_SIZE = 16384;

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
  sdFontSystem.releaseLoadedFont(renderer);

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
  state = State::SECTIONS;
  selectorIndex = 0;
  items.clear();
  requestUpdate();
}

void BookOrbitCatalogActivity::loadContinueReading() {
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) requestUpdate(true);

  items.clear();
  const auto e = KOReaderCatalogClient::fetchContinueReading(items);
  if (e != KOReaderCatalogClient::OK) {
    showError(catalogErr(e));
    return;
  }
  selectorIndex = 0;
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
  return "/books/" + StringUtils::sanitizeFilename(base) + ".epub";
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
  requestUpdate(true);

  const std::string url = KOReaderCatalogClient::downloadUrl(detail.primaryFileId);
  const std::string dest = destPathForDetail();
  LOG_DBG("BOCAT", "Downloading fileId=%d -> %s", detail.primaryFileId, dest.c_str());

  auto pollCancel = [this] {
    if (cancelRequested) return true;
    mappedInput.update();
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelRequested = true;
    }
    return cancelRequested;
  };

  HttpDownloader::DownloadOptions opts;
  opts.shouldCancel = pollCancel;
  opts.bufferSize = CATALOG_DOWNLOAD_BUFFER_SIZE;
  opts.authMode = HttpDownloader::AuthMode::KosyncHeader;

  const auto result = HttpDownloader::downloadToFile(
      url, dest,
      [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        requestUpdate(true);
      },
      &cancelRequested, KOREADER_STORE.getUsername(), KOREADER_STORE.getMd5Password(), opts);

  if (result == HttpDownloader::OK) {
    clearBookCache(dest);
    downloadedPath = dest;
    state = State::DETAIL;
  } else if (result == HttpDownloader::ABORTED) {
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
    }
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::SECTIONS) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.fillRect(0, 60 + selectorIndex * 30 - 2, pageWidth - 1, 30);
    for (int i = 0; i < SECTION_COUNT; i++) {
      renderer.drawText(UI_10_FONT_ID, 20, 60 + i * 30, sectionTitle(i).c_str(), i != selectorIndex);
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
