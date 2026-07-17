#pragma once
#include <string>
#include <vector>

/**
 * Client for BookOrbit's KOReader catalog API
 * (GET/PUT under <base>/koreader/plugin/catalog/*), the same API the official
 * BookOrbit KOReader plugin's in-app library browser uses. Lets CrossInk browse
 * the server's whole library and pull books over WiFi.
 *
 * Auth reuses the kosync header credentials (x-auth-user + x-auth-key=md5(pw))
 * via HttpDownloader, and the base URL comes from KOReaderCredentialStore
 * (KOREADER_STORE.getBaseUrl(), which already ends in /api/v1/koreader — the
 * catalog lives at /plugin/catalog under it).
 *
 * Only supported by BookOrbit servers; plain kosync servers 404 these routes,
 * which surfaces as a fetch failure the caller reports as "unavailable".
 */

// A single book row from a catalog list or the dashboard.
struct BookOrbitCatalogItem {
  int id = 0;
  std::string title;
  std::string author;      // first author, joined for display
  std::string seriesName;  // may be empty
  std::string readStatus;  // unread|reading|read|...
  float progressPercentage = -1.0f;  // <0 = unknown
  bool hasCover = false;
  int primaryFileId = 0;    // filled by detail fetch; 0 until then
  std::string primaryFormat;
};

// Paged result of a book-list query.
struct BookOrbitCatalogPage {
  std::vector<BookOrbitCatalogItem> items;
  int page = 1;
  int total = 0;
  bool hasNext = false;
};

// Dashboard summary stats (from /catalog/dashboard) for the landing screen.
struct BookOrbitDashboard {
  int currentStreak = -1;   // consecutive reading days; <0 = unknown
  int longestStreak = -1;
  int goalBooks = -1;       // reading-goal target for the year; <0 = no goal
  int goalCompleted = -1;   // books completed toward the goal
  int goalYear = 0;
  int totalBooks = -1;      // total books in the library; <0 = unknown
  std::string displayName;  // server-side display name (may be empty)
};

// Detail for one book, including the downloadable primary file.
struct BookOrbitCatalogDetail {
  int id = 0;
  std::string title;
  std::string author;
  std::string seriesName;
  int seriesIndex = -1;
  int seriesId = 0;              // >0 if part of a series (enables next-in-series)
  std::string readStatus;
  float progressPercentage = -1.0f;
  int primaryFileId = 0;         // EPUB (role=primary) file id, 0 if none
  std::string primaryFormat;
  long primarySizeBytes = 0;
  std::string description;       // truncated
  bool hasCover = false;
  // "Next in series" resolved from relatedSections (0 if none / already have all).
  int nextInSeriesId = 0;
  std::string nextInSeriesTitle;
};

// Query parameters for a book-list fetch. All optional; empty/zero = omit.
struct BookOrbitBooksQuery {
  std::string sort;         // "title" | "recently_added" | "author" | "series"
  std::string search;       // ?q= substring
  std::string readStatus;   // ?readStatus= reading|finished|unread|abandoned
  int seriesId = 0;         // ?seriesId= (books within a series)
  int page = 1;
};

class KOReaderCatalogClient {
 public:
  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,   // fetch failed / server unreachable
    UNAVAILABLE,     // server has no catalog endpoint (plain kosync) -> 404
    PARSE_ERROR,
  };

  // Dashboard "Continue Reading" list (books in progress). Small; one call.
  static Error fetchContinueReading(std::vector<BookOrbitCatalogItem>& out);

  // Fetch the full dashboard in one call: summary stats + the Continue Reading
  // list. Feeds the landing screen (streak, reading goal, total books).
  static Error fetchDashboard(BookOrbitDashboard& stats, std::vector<BookOrbitCatalogItem>& continueReading);

  // Paginated all-books list. sort is a catalog sort key (e.g. "title",
  // "recently_added"); page is 1-based.
  static Error fetchBooks(const std::string& sort, int page, BookOrbitCatalogPage& out);

  // Paginated book list with full query support (sort + search + read-status
  // filter + series filter). Empty/zero query fields are omitted from the URL.
  static Error fetchBooks(const BookOrbitBooksQuery& query, BookOrbitCatalogPage& out);

  // Full detail for one book (needed to resolve the primary EPUB file id/size).
  static Error fetchDetail(int bookId, BookOrbitCatalogDetail& out);

  // Absolute URL to download a file's bytes (feed to HttpDownloader).
  static std::string downloadUrl(int fileId);

  // Absolute URL for a book's cover thumbnail.
  static std::string thumbnailUrl(int bookId);

  // Write reading status back to the server (closes the on-device
  // Mark-as-Finished sync gap). status in {reading, finished, abandoned}.
  static Error setReadStatus(int bookId, const std::string& status);

  // Download a book file to destPath on the SD card, using the same insecure-TLS
  // WiFiClientSecure stack as kosync (no CA bundle -> fits the C3 heap; the
  // esp_http_client path can't do insecure TLS without a build flag). onProgress
  // is called with (downloaded, total) bytes; cancelFlag (if non-null) aborts
  // when it becomes true. knownTotal (>0) is used as the progress total when the
  // server sends no Content-Length (BookOrbit streams without one). Returns OK on
  // a complete download.
  static Error downloadFile(int fileId, const std::string& destPath,
                            void (*onProgress)(size_t, size_t, void*) = nullptr, void* progressCtx = nullptr,
                            const bool* cancelFlag = nullptr, long knownTotal = 0);

  // Download a book's cover thumbnail (JPEG) to destPath on the SD card, using
  // the same insecure-TLS stack as downloadFile. Small (~50 KB) and only fetched
  // on the detail screen (heap at baseline, not the handshake trough). Returns OK
  // on success; caller converts JPEG->BMP for display.
  static Error downloadThumbnail(int bookId, const std::string& destPath);

  static const char* errorString(Error e);

 private:
  // Base for all catalog routes: KOREADER_STORE.getBaseUrl() + "/plugin/catalog".
  static std::string catalogBase();
  // GET url into outBody using kosync header auth; classifies 404 as UNAVAILABLE.
  static Error httpGetJson(const std::string& url, std::string& outBody);
  // Shared HTTP-to-SD download core (insecure TLS, chunked-decode via
  // writeToStream). Used by downloadFile + downloadThumbnail.
  static Error downloadUrlToFile(const std::string& url, const std::string& destPath,
                                 void (*onProgress)(size_t, size_t, void*), void* progressCtx,
                                 const bool* cancelFlag, long knownTotal);
};
