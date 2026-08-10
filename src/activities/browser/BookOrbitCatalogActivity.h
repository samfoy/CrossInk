#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "network/KOReaderCatalogClient.h"
#include "util/ButtonNavigator.h"

/**
 * Browse and download from a BookOrbit library over WiFi, using BookOrbit's
 * KOReader catalog API (see KOReaderCatalogClient).
 *
 * Screen flow:
 *   SECTIONS  - landing menu: dashboard stats + the browse modes below
 *   SCOPES    - the server's saved SmartScopes (this is the to-be-read queue:
 *               selecting "Want to Read" lists exactly those books)
 *   LIST      - a page of books for the active query
 *   DETAIL    - one book: metadata, download, mark-finished
 *
 * INPUT: built on the FreeInkUI touch idiom, NOT the X3's ButtonNavigator-only
 * pattern. The X4 Pro's board profile assigns only two physical nav keys plus
 * power ({back,confirm,left,right} are all PIN_UNASSIGNED); Back and Confirm
 * come from the GT911 touch panel and the capacitive Home key. A list driven
 * solely by Up/Down/Confirm compiles fine and is unusable on this hardware, so
 * touch is the primary input here and the two nav keys are a secondary path.
 */
class BookOrbitCatalogActivity final : public Activity {
 public:
  explicit BookOrbitCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class View {
    CHECK_WIFI,
    WIFI_SELECTION,
    LOADING,
    SECTIONS,
    SCOPES,
    LIST,
    DETAIL,
    DOWNLOADING,
    SEARCH_INPUT,
    ERROR,
  };

  // Browse modes offered on the SECTIONS landing screen. Each maps to a
  // BookOrbitBooksQuery; SMART_SCOPES opens the scope picker instead.
  enum class BrowseMode {
    CONTINUE_READING,   // cached dashboard list, no extra round-trip
    SMART_SCOPES,       // -> SCOPES screen (Want to Read / Up Next in Series / ...)
    ALL_RECENT,         // sort=recently_added
    ALL_TITLE,          // sort=title
    CURRENTLY_READING,  // readStatus=reading
    UNREAD,             // readStatus=unread
    FINISHED,           // readStatus=finished
    SEARCH,             // prompt for text, then q=
  };
  static constexpr int BROWSE_MODE_COUNT = 8;

  // 24 interaction slots covers the densest page plus header controls, matching
  // the sizing OpdsBookBrowserActivity settled on.
  using UiApp = freeink::ui::FreeInkApp<24, 6>;

  // --- state ----------------------------------------------------------------
  View view = View::CHECK_WIFI;
  View returnView = View::SECTIONS;  // where DETAIL/errors go back to

  BookOrbitDashboard dashboard;
  std::vector<BookOrbitCatalogItem> continueReadingCache;
  std::vector<BookOrbitSmartScope> scopes;
  BookOrbitCatalogPage page;
  BookOrbitCatalogDetail detail;

  BrowseMode browseMode = BrowseMode::CONTINUE_READING;
  BookOrbitBooksQuery activeQuery;  // re-sent for paging
  std::string activeTitle;          // header for the LIST screen
  std::string searchTerm;
  std::string statusMessage;
  std::string errorMessage;

  int selectorIndex = 0;
  int visibleRows = 1;
  int topIndex = 0;
  bool consumeConfirm = false;
  bool consumeBack = false;

  // Download progress, written by the client's progress callback.
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  bool cancelDownload = false;
  std::string downloadTitle;

  // Drives the two physical nav keys (the only buttons this board has
  // besides power); touch is handled by the FreeInkApp.
  ButtonNavigator buttonNavigator;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the interaction table; loop() only routes touch snapshots
  // against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};

  // --- screen builders (run on the render task) ------------------------------
  static void rootScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  void screenHeader(UiApp::ScreenType& screen, const char* title);
  void buildSectionsScreen(UiApp::ScreenType& screen);
  void buildScopesScreen(UiApp::ScreenType& screen);
  void buildListScreen(UiApp::ScreenType& screen);
  void buildDetailScreen(UiApp::ScreenType& screen);
  void buildDownloadScreen(UiApp::ScreenType& screen);
  void buildStatusScreen(UiApp::ScreenType& screen);

  // --- flow -----------------------------------------------------------------
  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void loadDashboard();
  void enterSections();
  void selectBrowseMode(BrowseMode mode);
  void loadScopes();
  void openScope(int index);
  void loadQuery(const BookOrbitBooksQuery& query, int pageNumber);
  void openDetail(int bookId);
  void downloadCurrentBook();
  void markCurrentBookFinished();
  void launchSearch();
  void activateSelected();
  void goBack();
  void showError(const std::string& message);
  int currentRowCount() const;

  bool preventAutoSleep() override { return true; }
};
