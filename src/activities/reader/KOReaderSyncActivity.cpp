#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>
#include <ctime>

#include "CrossPointSettings.h"
#include "ClippingStore.h"
#include "clippings/ClippingsManager.h"
#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "ReadingStatsUtils.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
void syncTimeWithNTP() {
  // Stop SNTP if already running (can't reconfigure while running)
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  // Configure SNTP
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  // Wait for time to sync (with timeout)
  int retry = 0;
  const int maxRetries = 50;  // 5 seconds max
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < maxRetries) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    retry++;
  }

  if (retry < maxRetries) {
    LOG_DBG("KOSync", "NTP time synced");
  } else {
    LOG_DBG("KOSync", "NTP sync timeout, using fallback");
  }
}

void wifiOff() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}
}  // namespace

void KOReaderSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    LOG_DBG("KOSync", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
    epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    epub->setupCacheDir();
    // Load metadata only (no CSS needed for progress mapping, don't rebuild if cache is missing).
    if (!epub->load(false, true)) {
      LOG_ERR("KOSync", "Failed to load epub for progress mapping");
      epub.reset();
      return;
    }
    LOG_DBG("KOSync", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  }
}

void KOReaderSyncActivity::saveProgressAndReturn(const CrossPointPosition& position) {
  // epub is guaranteed non-null here: ensureEpubLoaded() was called in performSync() before
  // SHOWING_RESULT state is entered, and this method is only called from that state.
  assert(epub);
  const int pageCount = std::max(position.totalPages, position.pageNumber + 1);
  if (pageCount != position.totalPages) {
    LOG_DBG("KOSync", "Adjusted remote page count before save: page=%d count=%d -> %d", position.pageNumber,
            position.totalPages, pageCount);
  }
  if (!EpubReaderUtils::saveProgress(*epub, position.spineIndex, position.pageNumber, pageCount)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  // Reading happened regardless of sync direction: flush buffered page-stats
  // while WiFi is still up (onExit -> silent reboot tears the radio down).
  uploadPageStats();
  uploadAnnotations();
  downloadAnnotations();
  returnToReader();
}

void KOReaderSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

bool KOReaderSyncActivity::consumeInitialConfirmRelease() {
  if (!lockInitialConfirmRelease) {
    return false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    lockInitialConfirmRelease = false;
  }
  return true;
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    returnToReader();
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");
  sdFontSystem.releaseForNetwork(renderer);

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);

  // Sync time with NTP before making API requests
  syncTimeWithNTP();

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  performSync();
}

void KOReaderSyncActivity::performSync() {
  // Calculate document hash based on user's preferred method
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
    documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
  } else {
    documentHash = KOReaderDocumentId::calculate(epubPath);
  }
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }

  LOG_DBG("KOSync", "Document hash: %s", documentHash.c_str());

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("KOSync", "Fetch progress screen could not be rendered synchronously; aborting sync");
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  // Fetch remote progress
  const auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);

  if (result == KOReaderSyncClient::NOT_FOUND) {
    // No remote progress - offer to upload
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  // Epub was released before sync to free RAM for the TLS handshake — reload it now.
  hasRemoteProgress = true;
  ensureEpubLoaded();
  if (!epub) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }

  KOReaderPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
  remotePosition = ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, totalPagesInSpine);

  // Refine page using section cache LUTs: li index, anchor, or paragraph index.
  if (remotePosition.hasLiIndex || remotePosition.xpathAnchorId[0] != '\0' || remotePosition.hasParagraphIndex) {
    Section tempSection(epub, remotePosition.spineIndex, renderer);
    bool refined = false;
    if (remotePosition.hasLiIndex) {
      const auto liPage = tempSection.getPageForListItemIndex(remotePosition.liIndex);
      if (liPage.has_value()) {
        LOG_DBG("KOSync", "Li index %u -> page %d (was %d)", remotePosition.liIndex, *liPage,
                remotePosition.pageNumber);
        remotePosition.pageNumber = *liPage;
        refined = true;
      } else {
        LOG_DBG("KOSync", "Li index %u not found in section LUT", remotePosition.liIndex);
      }
    }
    if (!refined && remotePosition.xpathAnchorId[0] != '\0') {
      const auto anchorPage = tempSection.getPageForAnchor(std::string(remotePosition.xpathAnchorId));
      if (anchorPage.has_value()) {
        LOG_DBG("KOSync", "Anchor '%s' -> page %d (was %d)", remotePosition.xpathAnchorId, *anchorPage,
                remotePosition.pageNumber);
        remotePosition.pageNumber = *anchorPage;
        refined = true;
      } else {
        LOG_DBG("KOSync", "Anchor '%s' not found in section cache", remotePosition.xpathAnchorId);
      }
    }
    if (!refined && remotePosition.hasParagraphIndex) {
      const auto paragraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex);
      const auto nextParagraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex + 1);
      if (paragraphPage.has_value()) {
        int refinedPage = std::max(remotePosition.pageNumber, static_cast<int>(*paragraphPage));
        if (nextParagraphPage.has_value()) {
          const int lutSpan = static_cast<int>(*nextParagraphPage) - static_cast<int>(*paragraphPage);
          // Keep the percentage-derived page inside the paragraph's cached page range.
          // A one-page paragraph should not allow byte-percentage drift to jump to later paragraphs.
          if (lutSpan > 0 && refinedPage >= static_cast<int>(*nextParagraphPage)) {
            refinedPage = static_cast<int>(*nextParagraphPage) - 1;
          }
        }
        char nextParaBuf[8];
        if (nextParagraphPage.has_value())
          snprintf(nextParaBuf, sizeof(nextParaBuf), "%d", *nextParagraphPage);
        else
          snprintf(nextParaBuf, sizeof(nextParaBuf), "none");
        LOG_DBG("KOSync", "Paragraph %u -> LUT page %d, nextPara page %s, intra page %d, using %d",
                remotePosition.paragraphIndex, *paragraphPage, nextParaBuf, remotePosition.pageNumber, refinedPage);
        remotePosition.pageNumber = refinedPage;
      } else {
        LOG_DBG("KOSync", "Paragraph %u not found in section LUT", remotePosition.paragraphIndex);
      }
    }
  }
  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;

    // Default to the option that corresponds to the furthest progress
    if (localProgress.percentage > remoteProgress.percentage) {
      selectedOption = 1;  // Upload local progress
    } else {
      selectedOption = 0;  // Apply remote progress
    }
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("KOSync", "Upload progress screen could not be rendered synchronously; aborting upload");
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  if (epub) {
    epub.reset();
    LOG_DBG("KOSync", "Released epub before upload (heap: %u)", (unsigned)ESP.getFreeHeap());
  }

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;
  progress.device = SETTINGS.getEffectiveDeviceName();

  const auto result = KOReaderSyncClient::updateProgress(progress);

  // While WiFi is still up, also upload any buffered page-turn events to a
  // BookOrbit page-stats endpoint. This is what feeds the reading streak / time
  // / pace / DNA stats (plain KOSync progress does not create sessions). Best
  // effort: failures here never fail the progress sync. On the public
  // sync.koreader.rocks (no such endpoint) this 404s and we simply keep the
  // buffer for a future BookOrbit sync.
  if (result == KOReaderSyncClient::OK) {
    uploadPageStats();
    uploadAnnotations();
    downloadAnnotations();
  }

  // Drop the radio while user reads the result; full teardown happens at silent reboot.
  wifiOff();

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::uploadPageStats() {
  if (!SETTINGS.shouldUploadReadingStats()) {
    LOG_INF("KOSync", "page-stats: upload-stats toggle off, skipping");
    return;  // opt-in feature disabled
  }

  KOReaderPageStatsStore store;
  const bool loaded = store.load(documentHash);
  if (!loaded || store.empty()) {
    LOG_INF("KOSync", "page-stats: nothing to upload (loaded=%d empty=%d hash=%s)", loaded ? 1 : 0,
            store.empty() ? 1 : 0, documentHash.c_str());
    return;  // nothing buffered for this book
  }
  LOG_INF("KOSync", "page-stats: uploading %u buffered events", (unsigned)store.size());

  // Mint a device-local wall-clock string for the server (KOReader datetimes
  // carry no timezone). Best effort; empty is acceptable (server falls back).
  std::string deviceTime;
  ReadingStatsDateTime dt;
  if (getCurrentLocalReadingStatsDateTime(dt) && dt.isValid()) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", dt.date.year, dt.date.month, dt.date.day, dt.hour,
             dt.minute, dt.second);
    deviceTime = buf;
  }

  const std::string model = SETTINGS.getEffectiveDeviceName();
  const auto res = KOReaderSyncClient::uploadPageStats(model, store, deviceTime);
  if (res == KOReaderSyncClient::OK) {
    store.clear();  // remove the on-disk buffer only after the server accepted it
    LOG_INF("KOSync", "Page-stats uploaded and buffer cleared");
  } else if (res == KOReaderSyncClient::NOT_FOUND) {
    // The server doesn't implement /plugin/page-stats (e.g. sync.koreader.rocks
    // or kosync-dotnet). Drop the buffer so it can't accumulate to the cap and
    // re-fire a doomed multi-request upload on every future sync. (Progress
    // still syncs fine via kosync.) Re-enabling against a BookOrbit server later
    // simply starts buffering fresh.
    store.clear();
    LOG_INF("KOSync", "Server has no page-stats endpoint (404); discarded buffered stats");
  } else {
    // Transient failure (network/auth/500): keep the buffer for a later retry
    // (idempotent server-side, so overlap is safe).
    LOG_DBG("KOSync", "Page-stats upload failed, will retry (err=%d, http=%d)", static_cast<int>(res),
            KOReaderSyncClient::lastHttpCode);
  }
}

void KOReaderSyncActivity::uploadAnnotations() {
  if (!SETTINGS.shouldUploadReadingStats()) {
    return;  // gated behind the same opt-in as page-stats
  }
  if (documentHash.size() != 32) return;

  std::vector<Clipping> clippings;
  if (!ClippingStore::readForBook(epubPath, "epub", clippings) || clippings.empty()) {
    return;  // no highlights for this book
  }

  // Map CrossInk clippings to the server's annotation shape. CrossInk has no
  // KOReader DOM xpointers, so pos0 is a synthetic-but-STABLE locator built from
  // the clipping's spine/page/word position. The dedup key is md5(datetime|pos0),
  // so datetime must also be stable across syncs — we derive it deterministically
  // from pos0 (a fixed epoch base + an offset hashed from the locator). This keeps
  // re-syncs idempotent (server reports them "unchanged") at the cost of the
  // displayed date not being the real highlight time (Phase 1 tradeoff).
  std::vector<KOReaderSyncClient::AnnotationUpload> uploads;
  uploads.reserve(clippings.size());
  for (const Clipping& c : clippings) {
    if (c.text.empty()) continue;
    KOReaderSyncClient::AnnotationUpload a;
    char pos[64];
    snprintf(pos, sizeof(pos), "/crossink/%u/%u/%u", static_cast<unsigned>(c.spineIndex),
             static_cast<unsigned>(c.startPage), static_cast<unsigned>(c.startWordIndex));
    a.pos0 = pos;
    a.pageno = static_cast<int>(c.startPage);
    a.text = c.text;
    a.chapter = c.chapterTitle;

    // Deterministic datetime from a stable FNV-1a hash of pos0, spread across a
    // ~10-year window from 2020-01-01 so the md5(datetime|pos0) key is stable.
    uint32_t h = 2166136261u;
    for (char ch : a.pos0) {
      h ^= static_cast<uint8_t>(ch);
      h *= 16777619u;
    }
    const uint32_t base = 1577836800u;              // 2020-01-01 00:00:00 UTC
    const uint32_t epoch = base + (h % 315360000u);  // + up to ~10 years
    time_t t = static_cast<time_t>(epoch);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char dt[24];
    snprintf(dt, sizeof(dt), "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    a.datetime = dt;
    uploads.push_back(std::move(a));
  }
  if (uploads.empty()) return;

  std::string deviceTime;
  ReadingStatsDateTime dtNow;
  if (getCurrentLocalReadingStatsDateTime(dtNow) && dtNow.isValid()) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", dtNow.date.year, dtNow.date.month, dtNow.date.day,
             dtNow.hour, dtNow.minute, dtNow.second);
    deviceTime = buf;
  }

  const std::string model = SETTINGS.getEffectiveDeviceName();
  const auto res = KOReaderSyncClient::uploadAnnotations(model, documentHash, uploads, deviceTime);
  if (res == KOReaderSyncClient::OK) {
    LOG_INF("KOSync", "Annotations uploaded (%u) for %s", (unsigned)uploads.size(), documentHash.c_str());
  } else if (res == KOReaderSyncClient::NOT_FOUND) {
    LOG_INF("KOSync", "Server has no annotations endpoint (404); skipping");
  } else {
    LOG_DBG("KOSync", "Annotation upload failed (err=%d, http=%d)", static_cast<int>(res),
            KOReaderSyncClient::lastHttpCode);
  }
}

void KOReaderSyncActivity::downloadAnnotations() {
  if (!SETTINGS.shouldUploadReadingStats()) return;  // same opt-in as upload
  if (documentHash.size() != 32) return;

  // Track which server annotations we've already merged, one serverId per line,
  // so re-syncs don't append duplicates into the clippings file.
  const std::string cursorDir = "/.crosspoint/annot_synced";
  const std::string cursorPath = cursorDir + "/" + documentHash + ".txt";
  std::string appliedSet;  // newline-delimited serverIds already merged
  {
    HalFile f;
    if (Storage.openFileForRead("KOAnnot", cursorPath, f)) {
      char buf[256];
      int n;
      while ((n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf))) > 0) appliedSet.append(buf, n);
      f.close();
    }
  }
  auto alreadyApplied = [&](int serverId) {
    const std::string needle = "\n" + std::to_string(serverId) + "\n";
    const std::string hay = "\n" + appliedSet + "\n";
    return hay.find(needle) != std::string::npos;
  };

  const std::string model = SETTINGS.getEffectiveDeviceName();
  std::string deviceTime;
  ReadingStatsDateTime dtNow;
  if (getCurrentLocalReadingStatsDateTime(dtNow) && dtNow.isValid()) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", dtNow.date.year, dtNow.date.month, dtNow.date.day,
             dtNow.hour, dtNow.minute, dtNow.second);
    deviceTime = buf;
  }

  // A book label for the clippings file. Reuse the existing clipping store's
  // recorded title/author for this book if present; else fall back to filename.
  std::string title = epubPath;
  std::string author;
  {
    std::vector<ClippedBookEntry> books;
    if (ClippingStore::getAllClippedBooks(books)) {
      for (const auto& b : books) {
        if (b.bookPath == epubPath) {
          title = b.bookTitle.empty() ? title : b.bookTitle;
          author = b.bookAuthor;
          break;
        }
      }
    }
  }

  int pulls = 0;
  bool more = true;
  std::vector<KOReaderSyncClient::AnnotationDownload> newlyApplied;
  // Bounded loop: the server paginates via `more`; cap iterations defensively.
  while (more && pulls < 20) {
    pulls++;
    std::vector<KOReaderSyncClient::AnnotationDownload> adds;
    const auto res = KOReaderSyncClient::exchangeAnnotations(model, documentHash, adds, more, deviceTime);
    if (res == KOReaderSyncClient::NOT_FOUND) {
      LOG_INF("KOSync", "Server has no annotation-exchange endpoint; skipping");
      return;
    }
    if (res != KOReaderSyncClient::OK) {
      LOG_DBG("KOSync", "Annotation exchange failed (err=%d, http=%d)", static_cast<int>(res),
              KOReaderSyncClient::lastHttpCode);
      return;
    }
    if (adds.empty()) break;
    for (const auto& a : adds) {
      if (alreadyApplied(a.serverId)) continue;
      // Merge into the on-device clippings file. CrossInk can't re-anchor the
      // server's DOM xpointer, but page + text + chapter + note are readable.
      std::string text = a.text;
      if (!a.note.empty()) text += "\n[note] " + a.note;
      const int page = a.pageno >= 0 ? a.pageno : 0;
      if (ClippingsManager::saveClipping(title, author, a.chapter, page, text)) {
        appliedSet += std::to_string(a.serverId) + "\n";
        newlyApplied.push_back(a);
      }
    }
  }

  if (!newlyApplied.empty()) {
    // Persist the cursor and ack the server so it advances its per-device cursor.
    Storage.ensureDirectoryExists(cursorDir.c_str());
    HalFile f;
    if (Storage.openFileForWrite("KOAnnot", cursorPath, f)) {
      f.write(reinterpret_cast<const uint8_t*>(appliedSet.data()), appliedSet.size());
      f.close();
    }
    KOReaderSyncClient::ackAnnotations(model, documentHash, newlyApplied, deviceTime);
    LOG_INF("KOSync", "Merged %u server annotations into clippings", (unsigned)newlyApplied.size());
  }
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  lockInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Past this point every path uses WiFi.
  sdFontSystem.releaseLoadedFont(renderer);
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth)
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection subactivity
  LOG_DBG("KOSync", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();

  if (wifiActivated) {
    wifiOff();
    silentRestartToReader();
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_KOREADER_SYNC));

  int top = screen.y + screen.height / 2 - 40;
  if (state == NO_CREDENTIALS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_KOREADER_SETUP_HINT), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    // Show comparison
    top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    // Remote chapter name requires Epub (loaded lazily in performSync before this state).
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    // Local chapter name was pre-computed before Epub was released.
    const std::string localChapter =
        !localChapterName.empty() ? localChapterName
                                  : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    // Remote progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 40, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 65, remoteChapterStr);
    char remotePageStr[64];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 90, remotePageStr);

    if (!remoteProgress.device.empty()) {
      char deviceStr[64];
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
      renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 115, deviceStr);
    }

    // Local progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 150, tr(STR_LOCAL_LABEL), true);
    char localChapterStr[128];
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 175, localChapterStr);
    char localPageStr[64];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 200, localPageStr);

    const int optionY = top + 230;
    const int optionHeight = 30;

    // Apply option
    if (selectedOption == 0) {
      renderer.fillRect(screen.x, optionY - 2, screen.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY, tr(STR_APPLY_REMOTE),
                      selectedOption != 0);

    // Upload option
    if (selectedOption == 1) {
      renderer.fillRect(screen.x, optionY + optionHeight - 2, screen.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY + optionHeight,
                      tr(STR_UPLOAD_LOCAL), selectedOption != 1);

    // Bottom button hints
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_UPLOAD_PROMPT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_UPLOAD_SUCCESS), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    const int messageWidth = screen.width - metrics.contentSidePadding * 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const auto messageLines = renderer.wrappedText(UI_10_FONT_ID, statusMessage.c_str(), messageWidth, 3);
    int messageY = top + 40;
    for (const auto& line : messageLines) {
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, messageY, line.c_str());
      messageY += lineHeight + 4;
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderSyncActivity::loop() {
  if (consumeInitialConfirmRelease()) {
    return;
  }

  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    // Navigate options
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        saveProgressAndReturn(remotePosition);
      } else if (selectedOption == 1) {
        // Upload local progress
        performUpload();
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Calculate hash if not done yet
      if (documentHash.empty()) {
        if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
          documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
        } else {
          documentHash = KOReaderDocumentId::calculate(epubPath);
        }
      }
      performUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }
}
