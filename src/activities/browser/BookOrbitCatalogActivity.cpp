#include "BookOrbitCatalogActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_CANCEL = 2;

// Rows on the DETAIL screen, in render order.
constexpr int DETAIL_ROW_DOWNLOAD = 0;
constexpr int DETAIL_ROW_MARK_FINISHED = 1;
constexpr int DETAIL_ROW_COUNT = 2;

// Progress percent shown for a book whose progress the server didn't report.
constexpr float PROGRESS_UNKNOWN = -1.0f;

// Server-settable read status for "finished". The catalog FILTER vocabulary
// ("finished") and the settable status enum ("read") are different sets --
// sending the filter word here is silently rejected.
constexpr char READ_STATUS_FINISHED[] = "read";

// Draw one line of text at the top of the remaining body. Screen<> has no
// text() primitive -- the firmware idiom is target().text() into a band taken
// off the layout cursor (see EpubReaderPercentSelectionActivity).
template <typename ScreenT>
void textLine(ScreenT& screen, const char* str, const fui::TextStyle& style, int16_t gap = 0) {
  if (str == nullptr || str[0] == '\0') return;
  const int16_t lh = screen.target().lineHeight(style.font);
  screen.target().text(screen.takeTop(lh, gap), str, style);
}

// Compose "Title" / "Title (Series)" for a row label.
std::string rowLabel(const BookOrbitCatalogItem& item) {
  if (item.seriesName.empty()) return item.title;
  return item.title + " (" + item.seriesName + ")";
}
}  // namespace

BookOrbitCatalogActivity::BookOrbitCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BookOrbitCatalog", renderer, mappedInput),
      buttonNavigator(),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void BookOrbitCatalogActivity::onEnter() {
  Activity::onEnter();

  view = View::CHECK_WIFI;
  selectorIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  consumeConfirm = false;
  consumeBack = false;
  errorMessage.clear();
  statusMessage = tr(STR_LOADING);

  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &BookOrbitCatalogActivity::onRowEvent, this);
  app.on(ACTION_CANCEL, &BookOrbitCatalogActivity::onCancelEvent, this);
  app.setScreen(&BookOrbitCatalogActivity::rootScreen, this);
  requestUpdate();

  checkAndConnectWifi();
}

void BookOrbitCatalogActivity::onExit() {
  uiReady = false;
  // Free the biggest buffers explicitly (reverse of acquisition) rather than
  // waiting for the activity's destruction, so the next activity starts with
  // the heap back.
  continueReadingCache.clear();
  continueReadingCache.shrink_to_fit();
  page.items.clear();
  page.items.shrink_to_fit();
  scopes.clear();
  scopes.shrink_to_fit();
  Activity::onExit();
}

int BookOrbitCatalogActivity::currentRowCount() const {
  switch (view) {
    case View::SECTIONS:
      return BROWSE_MODE_COUNT;
    case View::SCOPES:
      return static_cast<int>(scopes.size());
    case View::LIST:
      return static_cast<int>(page.items.size());
    case View::DETAIL:
      return DETAIL_ROW_COUNT;
    default:
      return 0;
  }
}

// --- render ------------------------------------------------------------------

void BookOrbitCatalogActivity::rootScreen(UiApp::ScreenType& screen, void* user) {
  auto* self = static_cast<BookOrbitCatalogActivity*>(user);
  switch (self->view) {
    case View::SECTIONS:
      self->buildSectionsScreen(screen);
      break;
    case View::SCOPES:
      self->buildScopesScreen(screen);
      break;
    case View::LIST:
      self->buildListScreen(screen);
      break;
    case View::DETAIL:
      self->buildDetailScreen(screen);
      break;
    case View::DOWNLOADING:
      self->buildDownloadScreen(screen);
      break;
    default:
      self->buildStatusScreen(screen);
      break;
  }
}

void BookOrbitCatalogActivity::screenHeader(UiApp::ScreenType& screen, const char* title) {
  screen.takeBottom(static_cast<int16_t>(UITheme::getInstance().getMetrics().buttonHintsHeight));
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().topPadding));
  fui::HeaderProps header;
  header.title = title;
  screen.header(header);
}

void BookOrbitCatalogActivity::buildSectionsScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, tr(STR_BOOKORBIT_LIBRARY));

  // Library summary. Streak/goal are deliberately NOT shown: BookOrbit v2.4.0
  // stopped sending them to kosync clients, and rendering an unknown value as
  // "0 day streak" would be a lie. Only what the server actually returns.
  char summary[96] = {0};
  if (dashboard.totalBooks >= 0) {
    if (dashboard.inProgress >= 0) {
      snprintf(summary, sizeof(summary), "%d %s  ·  %d %s", dashboard.totalBooks, tr(STR_BOOKS),
               dashboard.inProgress, tr(STR_IN_PROGRESS));
    } else {
      snprintf(summary, sizeof(summary), "%d %s", dashboard.totalBooks, tr(STR_BOOKS));
    }
    fui::TextStyle style = screen.theme().bodyText;
    style.align = fui::TextAlign::Center;
    textLine(screen, summary, style);
    screen.spacer(screen.theme().spaceSm);
  }

  const StrId labels[BROWSE_MODE_COUNT] = {
      StrId::STR_CONTINUE_READING, StrId::STR_SMART_SCOPES, StrId::STR_RECENTLY_ADDED, StrId::STR_ALL_BOOKS,
      StrId::STR_CURRENTLY_READING, StrId::STR_UNREAD, StrId::STR_FINISHED, StrId::STR_SEARCH,
  };

  std::vector<fui::ListItem> items;
  items.reserve(BROWSE_MODE_COUNT);
  for (int i = 0; i < BROWSE_MODE_COUNT; i++) {
    fui::ListItem item;
    item.label = I18N.get(labels[i]);
    item.value = ">";
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectorIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical nav keys stay in loop()
  props.valueInset = 8;
  const auto rows = fui::listVisibleRows(screen.body(), screen.theme().rowHeight, screen.theme().listRowGap);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, BROWSE_MODE_COUNT);
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void BookOrbitCatalogActivity::buildScopesScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, tr(STR_SMART_SCOPES));

  if (scopes.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  std::vector<fui::ListItem> items;
  items.reserve(scopes.size());
  for (const auto& scope : scopes) {
    fui::ListItem item;
    item.label = scope.title.c_str();
    item.value = ">";
    item.actionValue = static_cast<int16_t>(items.size());
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectorIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  const auto rows = fui::listVisibleRows(screen.body(), screen.theme().rowHeight, screen.theme().listRowGap);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(scopes.size()));
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void BookOrbitCatalogActivity::buildListScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, activeTitle.empty() ? tr(STR_BOOKORBIT_LIBRARY) : activeTitle.c_str());

  if (page.items.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  // Per-render owned strings; items point into them for the draw only.
  std::vector<std::string> labels;
  std::vector<std::string> values;
  labels.reserve(page.items.size());
  values.reserve(page.items.size());
  std::vector<fui::ListItem> items;
  items.reserve(page.items.size());
  for (const auto& book : page.items) {
    labels.push_back(rowLabel(book));
    // Show reading progress where the server reported it; blank otherwise so an
    // unknown value never renders as 0%.
    if (book.progressPercentage > PROGRESS_UNKNOWN) {
      char pct[8];
      snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(book.progressPercentage + 0.5f));
      values.emplace_back(pct);
    } else {
      values.emplace_back();
    }
    fui::ListItem item;
    item.label = labels.back().c_str();
    if (!book.author.empty()) item.subtitle = book.author.c_str();
    if (!values.back().empty()) item.value = values.back().c_str();
    item.actionValue = static_cast<int16_t>(items.size());
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectorIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  const auto rows = fui::listVisibleRows(screen.body(), screen.theme().rowHeight, screen.theme().listRowGap);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(page.items.size()));
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void BookOrbitCatalogActivity::buildDetailScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, tr(STR_BOOK_DETAILS));

  fui::TextStyle body = screen.theme().bodyText;
  textLine(screen, detail.title.c_str(), body);
  if (!detail.author.empty()) textLine(screen, detail.author.c_str(), body);
  if (!detail.seriesName.empty()) {
    char series[96];
    if (detail.seriesIndex > 0) {
      snprintf(series, sizeof(series), "%s #%d", detail.seriesName.c_str(), detail.seriesIndex);
    } else {
      snprintf(series, sizeof(series), "%s", detail.seriesName.c_str());
    }
    textLine(screen, series, body);
  }
  if (detail.progressPercentage > PROGRESS_UNKNOWN) {
    char pct[32];
    snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(detail.progressPercentage + 0.5f));
    textLine(screen, pct, body);
  }
  screen.spacer(screen.theme().spaceMd);

  std::vector<fui::ListItem> items;
  items.reserve(DETAIL_ROW_COUNT);
  fui::ListItem download;
  download.label = tr(STR_DOWNLOAD);
  download.actionValue = DETAIL_ROW_DOWNLOAD;
  // No EPUB on the server => nothing to fetch; say so instead of failing later.
  if (detail.primaryFileId == 0) download.value = tr(STR_NOT_SET);
  items.push_back(download);

  fui::ListItem finish;
  finish.label = tr(STR_MARK_AS_FINISHED);
  finish.actionValue = DETAIL_ROW_MARK_FINISHED;
  items.push_back(finish);

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectorIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  const auto rows = fui::listVisibleRows(screen.body(), screen.theme().rowHeight, screen.theme().listRowGap);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = 0;
  props.topIndex = 0;
  screen.list(props);
}

void BookOrbitCatalogActivity::buildDownloadScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, tr(STR_DOWNLOADING));

  fui::TextStyle centered = screen.theme().bodyText;
  centered.align = fui::TextAlign::Center;
  screen.spacer(screen.theme().spaceMd);
  textLine(screen, downloadTitle.c_str(), centered);
  screen.spacer(screen.theme().spaceSm);

  char line[48];
  if (downloadTotal > 0) {
    const int pct = static_cast<int>((static_cast<uint64_t>(downloadProgress) * 100ULL) / downloadTotal);
    snprintf(line, sizeof(line), "%d%%  (%u KB)", pct, static_cast<unsigned>(downloadProgress / 1024));
  } else {
    // BookOrbit streams without Content-Length, so a total isn't always known.
    // Show bytes moved rather than a bar that can't be filled.
    snprintf(line, sizeof(line), "%u KB", static_cast<unsigned>(downloadProgress / 1024));
  }
  textLine(screen, line, centered);
  screen.spacer(screen.theme().spaceMd);

  fui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = ACTION_CANCEL;
  cancel.inputMask = fui::InputTouch;
  screen.button(cancel);
}

void BookOrbitCatalogActivity::buildStatusScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, tr(STR_BOOKORBIT_LIBRARY));
  const char* message = view == View::ERROR ? errorMessage.c_str() : statusMessage.c_str();
  screen.centeredText(message, screen.theme().bodyText);
}

void BookOrbitCatalogActivity::render(RenderLock&& lock) {
  uiReady = false;
  renderer.clearScreen();
  app.render();
  // mapLabels routes the hints onto whatever buttons this board actually has --
  // essential on the X4 Pro, which has no physical Back/Confirm at all.
  MappedInputManager::Labels labels;
  switch (view) {
    case View::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case View::ERROR:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
    case View::SECTIONS:
    case View::SCOPES:
    case View::LIST:
    case View::DETAIL:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      break;
    default:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  uiReady = true;
}

// --- input -------------------------------------------------------------------

void BookOrbitCatalogActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookOrbitCatalogActivity*>(user);
  const int index = static_cast<int>(event.value);
  if (index < 0 || index >= self->currentRowCount()) return;
  self->selectorIndex = index;
  self->activateSelected();
}

void BookOrbitCatalogActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<BookOrbitCatalogActivity*>(user);
  self->cancelDownload = true;
}

void BookOrbitCatalogActivity::loop() {
  if (view == View::WIFI_SELECTION || view == View::SEARCH_INPUT) return;

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  if (view == View::ERROR) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (view == View::CHECK_WIFI || view == View::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  if (view == View::DOWNLOADING) {
    // The transfer runs synchronously inside downloadCurrentBook(); this only
    // sees input if the download already returned.
    return;
  }

  const int rowCount = currentRowCount();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    goBack();
    return;
  }

  // Touch is the PRIMARY input on this device: the X4 Pro has no physical
  // Back/Confirm/Left/Right at all. render() registered every row as a tap
  // target; route the snapshot and let the handlers dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  if (rowCount <= 0) return;

  // Swipes scroll the viewport; the selection stays put and the two physical nav
  // keys pull the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(topIndex, delta, visibleRows, rowCount);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  const auto moveSelection = [this, rowCount](const int index) {
    selectorIndex = index;
    topIndex = followListSelection(selectorIndex, topIndex, visibleRows, rowCount);
    requestUpdate();
  };
  buttonNavigator.onNextRelease(
      [this, rowCount, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectorIndex, rowCount)); });
  buttonNavigator.onPreviousRelease(
      [this, rowCount, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectorIndex, rowCount)); });
  buttonNavigator.onNextContinuous([this, rowCount, &moveSelection] {
    moveSelection(ButtonNavigator::nextPageIndex(selectorIndex, rowCount, visibleRows));
  });
  buttonNavigator.onPreviousContinuous([this, rowCount, &moveSelection] {
    moveSelection(ButtonNavigator::previousPageIndex(selectorIndex, rowCount, visibleRows));
  });
}

void BookOrbitCatalogActivity::activateSelected() {
  switch (view) {
    case View::SECTIONS:
      selectBrowseMode(static_cast<BrowseMode>(selectorIndex));
      return;
    case View::SCOPES:
      openScope(selectorIndex);
      return;
    case View::LIST:
      if (selectorIndex >= 0 && selectorIndex < static_cast<int>(page.items.size())) {
        openDetail(page.items[selectorIndex].id);
      }
      return;
    case View::DETAIL:
      if (selectorIndex == DETAIL_ROW_DOWNLOAD) {
        downloadCurrentBook();
      } else if (selectorIndex == DETAIL_ROW_MARK_FINISHED) {
        markCurrentBookFinished();
      }
      return;
    default:
      return;
  }
}

void BookOrbitCatalogActivity::goBack() {
  switch (view) {
    case View::DETAIL:
      view = returnView;
      selectorIndex = 0;
      topIndex = 0;
      requestUpdate();
      return;
    case View::LIST:
    case View::SCOPES:
      enterSections();
      return;
    case View::ERROR:
      view = returnView;
      requestUpdate();
      return;
    case View::SECTIONS:
    default:
      finish();
      return;
  }
}

// --- flow --------------------------------------------------------------------

void BookOrbitCatalogActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    loadDashboard();
    return;
  }
  launchWifiSelection();
}

void BookOrbitCatalogActivity::launchWifiSelection() {
  view = View::WIFI_SELECTION;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookOrbitCatalogActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    finish();
    return;
  }
  loadDashboard();
}

void BookOrbitCatalogActivity::loadDashboard() {
  view = View::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdateAndWait();

  const auto err = KOReaderCatalogClient::fetchDashboard(dashboard, continueReadingCache);
  if (err != KOReaderCatalogClient::Error::OK) {
    returnView = View::SECTIONS;
    showError(KOReaderCatalogClient::errorString(err));
    return;
  }
  enterSections();
}

void BookOrbitCatalogActivity::enterSections() {
  view = View::SECTIONS;
  selectorIndex = 0;
  topIndex = 0;
  requestUpdate();
}

void BookOrbitCatalogActivity::selectBrowseMode(const BrowseMode mode) {
  browseMode = mode;
  switch (mode) {
    case BrowseMode::CONTINUE_READING: {
      // Reuse the list fetched with the dashboard -- no second round-trip.
      page.items = continueReadingCache;
      page.page = 1;
      page.total = static_cast<int>(page.items.size());
      page.hasNext = false;
      activeTitle = tr(STR_CONTINUE_READING);
      view = View::LIST;
      selectorIndex = 0;
      topIndex = 0;
      requestUpdate();
      return;
    }
    case BrowseMode::SMART_SCOPES:
      loadScopes();
      return;
    case BrowseMode::SEARCH:
      launchSearch();
      return;
    case BrowseMode::ALL_RECENT: {
      BookOrbitBooksQuery q;
      q.sort = "recently_added";
      activeTitle = tr(STR_RECENTLY_ADDED);
      loadQuery(q, 1);
      return;
    }
    case BrowseMode::ALL_TITLE: {
      BookOrbitBooksQuery q;
      q.sort = "title";
      activeTitle = tr(STR_ALL_BOOKS);
      loadQuery(q, 1);
      return;
    }
    case BrowseMode::CURRENTLY_READING: {
      BookOrbitBooksQuery q;
      q.readStatus = "reading";
      q.sort = "recently_read";
      activeTitle = tr(STR_CURRENTLY_READING);
      loadQuery(q, 1);
      return;
    }
    case BrowseMode::UNREAD: {
      BookOrbitBooksQuery q;
      q.readStatus = "unread";
      q.sort = "title";
      activeTitle = tr(STR_UNREAD);
      loadQuery(q, 1);
      return;
    }
    case BrowseMode::FINISHED: {
      BookOrbitBooksQuery q;
      q.readStatus = "finished";
      q.sort = "title";
      activeTitle = tr(STR_FINISHED);
      loadQuery(q, 1);
      return;
    }
  }
}

void BookOrbitCatalogActivity::loadScopes() {
  view = View::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdateAndWait();

  const auto err = KOReaderCatalogClient::fetchSmartScopes(scopes);
  if (err != KOReaderCatalogClient::Error::OK) {
    returnView = View::SECTIONS;
    showError(KOReaderCatalogClient::errorString(err));
    return;
  }
  view = View::SCOPES;
  selectorIndex = 0;
  topIndex = 0;
  requestUpdate();
}

void BookOrbitCatalogActivity::openScope(const int index) {
  if (index < 0 || index >= static_cast<int>(scopes.size())) return;
  // A SmartScope is just a saved server-side filter, so it runs through the
  // ordinary books query. This is what makes "Want to Read" a to-be-read queue
  // with no special-casing -- and every other scope work for free.
  BookOrbitBooksQuery q;
  q.smartScopeId = scopes[index].id;
  q.sort = "title";
  activeTitle = scopes[index].title;
  loadQuery(q, 1);
}

void BookOrbitCatalogActivity::loadQuery(const BookOrbitBooksQuery& query, const int pageNumber) {
  activeQuery = query;
  activeQuery.page = pageNumber;
  view = View::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdateAndWait();

  const auto err = KOReaderCatalogClient::fetchBooks(activeQuery, page);
  if (err != KOReaderCatalogClient::Error::OK) {
    returnView = View::SECTIONS;
    showError(KOReaderCatalogClient::errorString(err));
    return;
  }
  view = View::LIST;
  selectorIndex = 0;
  topIndex = 0;
  requestUpdate();
}

void BookOrbitCatalogActivity::openDetail(const int bookId) {
  returnView = View::LIST;
  view = View::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdateAndWait();

  const auto err = KOReaderCatalogClient::fetchDetail(bookId, detail);
  if (err != KOReaderCatalogClient::Error::OK) {
    showError(KOReaderCatalogClient::errorString(err));
    return;
  }
  view = View::DETAIL;
  selectorIndex = DETAIL_ROW_DOWNLOAD;
  topIndex = 0;
  requestUpdate();
}

void BookOrbitCatalogActivity::downloadCurrentBook() {
  if (detail.primaryFileId == 0) {
    returnView = View::DETAIL;
    showError(tr(STR_NO_ENTRIES));
    return;
  }

  // Destination: the OPDS download folder if configured, else SD root, matching
  // how the OPDS browser places its downloads.
  const char* folder = SETTINGS.opdsDownloadFolder;
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    LOG_ERR("BOCAT", "cannot create %s; falling back to SD root", folder);
    haveFolder = false;
  }
  std::string destPath = haveFolder ? (std::string("/") + folder) : std::string();
  destPath += "/" + StringUtils::sanitizeFilename(detail.title) + ".epub";

  downloadTitle = detail.title;
  downloadProgress = 0;
  downloadTotal = static_cast<size_t>(detail.primarySizeBytes > 0 ? detail.primarySizeBytes : 0);
  cancelDownload = false;
  view = View::DOWNLOADING;
  // Render the download screen ONCE, synchronously, and wait for it before the
  // transfer starts. No render may run concurrently with the transfer's
  // allocations; the progress callback only updates counters.
  requestUpdateAndWait();

  const auto err = KOReaderCatalogClient::downloadFile(
      detail.primaryFileId, destPath,
      [](size_t done, size_t total, void* ctx) {
        auto* self = static_cast<BookOrbitCatalogActivity*>(ctx);
        self->downloadProgress = done;
        if (total > 0) self->downloadTotal = total;
      },
      this, &cancelDownload, downloadTotal);

  returnView = View::DETAIL;
  if (err == KOReaderCatalogClient::Error::CANCELLED) {
    view = View::DETAIL;
    requestUpdate();
    return;
  }
  if (err != KOReaderCatalogClient::Error::OK) {
    showError(KOReaderCatalogClient::errorString(err));
    return;
  }
  statusMessage = tr(STR_DOWNLOAD_COMPLETE);
  view = View::DETAIL;
  requestUpdate();
}

void BookOrbitCatalogActivity::markCurrentBookFinished() {
  if (detail.id == 0) return;
  view = View::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdateAndWait();

  // NOTE: "read", not the filter word "finished" -- the settable status enum and
  // the catalog filter vocabulary are different sets on the server.
  const auto err = KOReaderCatalogClient::setReadStatus(detail.id, READ_STATUS_FINISHED);
  returnView = View::DETAIL;
  if (err != KOReaderCatalogClient::Error::OK) {
    showError(KOReaderCatalogClient::errorString(err));
    return;
  }
  detail.readStatus = READ_STATUS_FINISHED;
  view = View::DETAIL;
  requestUpdate();
}

void BookOrbitCatalogActivity::launchSearch() {
  view = View::SEARCH_INPUT;
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH), searchTerm, 64, InputType::Text),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          enterSections();
          return;
        }
        const auto& kb = std::get<KeyboardResult>(result.data);
        if (kb.text.empty()) {
          enterSections();
          return;
        }
        searchTerm = kb.text;
        BookOrbitBooksQuery q;
        q.search = searchTerm;
        activeTitle = searchTerm;
        loadQuery(q, 1);
      });
}

void BookOrbitCatalogActivity::showError(const std::string& message) {
  errorMessage = message;
  view = View::ERROR;
  LOG_ERR("BOCAT", "%s", message.c_str());
  requestUpdate();
}
