#pragma once
#include <memory>
#include <string>
#include <vector>

#include "network/KOReaderCatalogClient.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Browse and download books from a BookOrbit server over WiFi, using its
 * KOReader catalog API (the same one the official BookOrbit KOReader plugin's
 * in-app library uses). Reuses the kosync credentials already configured for
 * progress sync; no separate server setup.
 *
 * v1 scope: two top-level sections — "Continue Reading" (dashboard) and
 * "All Books" (paginated) — then a book detail screen with Download (+ Open)
 * and a Mark-as-Finished action that writes read-status back to the server.
 *
 * Downloads go to /books/ on the SD card so the existing library scan picks
 * them up (and kosync/page-stats immediately match by document hash).
 */
class BookOrbitCatalogActivity final : public Activity {
 public:
  enum class State {
    CHECK_WIFI,
    WIFI_SELECTION,
    LOADING,
    SECTIONS,     // top-level browse menu (Continue Reading / sorts / filters / search)
    LIST,         // a list of books (from a chosen browse mode)
    DETAIL,       // one book: download / open / mark finished / next-in-series
    DOWNLOADING,
    ERROR,
  };

  // Browse modes shown on the landing menu. Each maps to a specific query
  // (or the cached Continue Reading list, or the search text-input flow).
  enum class BrowseMode {
    CONTINUE_READING,   // cached dashboard list
    ALL_TITLE,          // sort=title
    ALL_RECENT,         // sort=recently_added
    CURRENTLY_READING,  // readStatus=reading
    UNREAD,             // readStatus=unread
    FINISHED,           // readStatus=finished
    SEARCH,             // prompt for text, then q=
  };
  static constexpr int BROWSE_MODE_COUNT = 7;

  explicit BookOrbitCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookOrbitCatalog", renderer, mappedInput), buttonNavigator() {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  State state = State::CHECK_WIFI;

  int selectorIndex = 0;
  bool consumeConfirm = false;
  bool consumeBack = false;

  std::string statusMessage;
  std::string errorMessage;

  // Dashboard summary stats shown on the sections landing screen.
  BookOrbitDashboard dashboard;
  bool dashboardLoaded = false;
  std::vector<BookOrbitCatalogItem> continueReadingCache;

  // Current browse mode + list data.
  BrowseMode browseMode = BrowseMode::CONTINUE_READING;
  BookOrbitBooksQuery listQuery;   // the query backing the current LIST (for paging)
  std::string searchTerm;          // last search text
  std::vector<BookOrbitCatalogItem> items;
  int listPage = 1;
  bool listHasNext = false;
  int listTotal = 0;

  // Detail view.
  BookOrbitCatalogDetail detail;
  std::string downloadedPath;   // set once a download completes (this session)
  bool alreadyOnDevice = false; // file exists on SD from a prior download
  bool coverReady = false;      // a converted BMP cover is available on SD
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  bool cancelRequested = false;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);

  void enterSections();
  void selectBrowseMode(BrowseMode mode);
  void loadContinueReading();
  void loadQuery(const BookOrbitBooksQuery& query, int page);
  void launchSearch();
  void openBookDetail(int bookId);
  void fetchAndPrepareCover();
  void downloadCurrentBook();
  void downloadNextInSeries();
  void markCurrentBookFinished();
  void refreshDownloadedFlag();

  void showError(const std::string& msg);
  bool preventAutoSleep() override;

  // Build the on-SD destination path for a detail's primary file.
  std::string destPathForDetail() const;
  static std::string destPathFor(const std::string& title, const std::string& author);
};
