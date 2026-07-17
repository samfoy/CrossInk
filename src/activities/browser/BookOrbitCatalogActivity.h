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
    SECTIONS,     // top-level: Continue Reading / All Books
    LIST,         // a list of books (continue-reading or all-books page)
    DETAIL,       // one book: download / mark finished
    DOWNLOADING,
    ERROR,
  };

  enum class Section { CONTINUE_READING, ALL_BOOKS };

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

  // Current section + list data.
  Section section = Section::CONTINUE_READING;
  std::vector<BookOrbitCatalogItem> items;
  int listPage = 1;
  bool listHasNext = false;
  int listTotal = 0;

  // Detail view.
  BookOrbitCatalogDetail detail;
  std::string downloadedPath;   // set once a download completes
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  bool cancelRequested = false;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);

  void enterSections();
  void loadContinueReading();
  void loadAllBooks(int page);
  void openBookDetail(int bookId);
  void downloadCurrentBook();
  void markCurrentBookFinished();

  void showError(const std::string& msg);
  bool preventAutoSleep() override;

  // Build the on-SD destination path for a detail's primary file.
  std::string destPathForDetail() const;
};
