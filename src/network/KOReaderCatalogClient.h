#pragma once
#include <cstddef>
#include <string>
#include <vector>

/**
 * Client for BookOrbit's KOReader catalog API
 * (GET/PUT under <base>/koreader/plugin/catalog/*), the same API the official
 * BookOrbit KOReader plugin's in-app library browser uses. Lets CrossInk browse
 * the server's whole library and pull books over WiFi.
 *
 * Auth reuses the kosync header credentials (x-auth-user + x-auth-key=md5(pw)),
 * and the base URL comes from KOReaderCredentialStore (KOREADER_STORE.getBaseUrl(),
 * which already ends in /api/v1/koreader — the catalog lives at /plugin/catalog
 * under it).
 *
 * Only supported by BookOrbit servers; plain kosync servers 404 these routes,
 * which surfaces as UNAVAILABLE.
 *
 * Lives in src/network/ (not lib/) because it depends on app-layer headers.
 * PlatformIO compiles each lib/ directory in isolation with no -Isrc, so a
 * lib/ -> src/ include compiles under the simulator but fails on device builds.
 */

// A single book row from a catalog list or the dashboard.
struct BookOrbitCatalogItem {
  int id = 0;
  std::string title;
  std::string author;                // first author, joined for display
  std::string seriesName;            // may be empty
  std::string readStatus;            // unread|reading|read|want_to_read|...
  float progressPercentage = -1.0f;  // <0 = unknown
  bool hasCover = false;
  int primaryFileId = 0;  // filled by detail fetch; 0 until then
  std::string primaryFormat;
};

// Paged result of a book-list query.
struct BookOrbitCatalogPage {
  std::vector<BookOrbitCatalogItem> items;
  int page = 1;
  int total = 0;
  bool hasNext = false;
};

// One saved SmartScope (server-side saved filter). BookOrbit exposes these via
// /catalog/sections/smart-scopes; each maps to a books query with smartScopeId.
// This is how the to-be-read queue works: Sam's "Want to Read" scope.
struct BookOrbitSmartScope {
  int id = 0;
  std::string title;
};

// Dashboard summary for the landing screen.
//
// NOTE: BookOrbit v2.4.0 no longer sends streak/goal figures to kosync clients
// (they moved to a JWT-only dashboard widget). Those fields therefore stay at
// their <0 "unknown" values and MUST render as absent, never as 0 — showing
// "0 day streak" would be a lie. What the server still provides is the library
// totals below.
struct BookOrbitDashboard {
  int currentStreak = -1;  // consecutive reading days; <0 = unknown/not sent
  int longestStreak = -1;  // <0 = unknown/not sent
  int goalBooks = -1;      // reading-goal target; <0 = unknown/not sent
  int goalCompleted = -1;  // <0 = unknown/not sent
  int goalYear = 0;
  int totalBooks = -1;   // total books in the library; <0 = unknown
  int inProgress = -1;   // browseCounts.inProgress
  int authors = -1;      // browseCounts.authors
  int series = -1;       // browseCounts.series
  int collections = -1;  // browseCounts.collections
  int smartScopes = -1;  // browseCounts.smartScopes
  std::string displayName;

  bool hasStreak() const { return currentStreak >= 0; }
  bool hasGoal() const { return goalBooks > 0; }
};

// Detail for one book, including the downloadable primary file.
struct BookOrbitCatalogDetail {
  int id = 0;
  std::string title;
  std::string author;
  std::string seriesName;
  int seriesIndex = -1;
  int seriesId = 0;  // >0 if part of a series
  std::string readStatus;
  float progressPercentage = -1.0f;
  bool hasCover = false;
  int primarySizeBytes = 0;  // best-effort size for the progress display
  int primaryFileId = 0;     // EPUB (role=primary) file id, 0 if none
  std::string primaryFormat;
};

// Query parameters for a books listing.
struct BookOrbitBooksQuery {
  std::string sort;        // title|recently_added|recently_read|series|...
  std::string search;      // ?q=
  std::string readStatus;  // unread|reading|finished (server-validated set)
  int seriesId = 0;        // ?seriesId= (books within a series)
  int smartScopeId = 0;    // ?smartScopeId= (a saved SmartScope, e.g. Want to Read)
  int page = 1;
};

class KOReaderCatalogClient {
 public:
  enum class Error {
    OK,
    NO_CREDENTIALS,  // no kosync username/password stored
    UNAVAILABLE,     // 404/405/501 - server has no catalog API
    AUTH_FAILED,     // 401/403
    NETWORK,         // never connected / TLS / timeout
    PARSE,           // malformed JSON
    LOW_MEMORY,      // not enough heap to parse the response
    IO,              // SD write failure during download
    CANCELLED,       // user cancelled a download
  };

  // Dashboard + the "Continue Reading" list in one round-trip.
  static Error fetchDashboard(BookOrbitDashboard& stats, std::vector<BookOrbitCatalogItem>& continueReading);

  // Saved SmartScopes (server-side filters). Drives the to-be-read queue.
  static Error fetchSmartScopes(std::vector<BookOrbitSmartScope>& out);

  // A page of books matching `query`.
  static Error fetchBooks(const BookOrbitBooksQuery& query, BookOrbitCatalogPage& out);

  // Full detail for one book (includes the downloadable primary file id).
  static Error fetchDetail(int bookId, BookOrbitCatalogDetail& out);

  // Write the server's read status for a book (mark finished etc.).
  static Error setReadStatus(int bookId, const std::string& status);

  // Download a book file to `destPath`. `onProgress` may be null; `cancelFlag`
  // is polled between chunks so the UI can abort.
  static Error downloadFile(int fileId, const std::string& destPath,
                            void (*onProgress)(size_t, size_t, void*) = nullptr, void* progressCtx = nullptr,
                            const bool* cancelFlag = nullptr, size_t knownTotalBytes = 0);

  static std::string thumbnailUrl(int bookId);
  static const char* errorString(Error e);

  // Max SmartScopes kept from the server. Sam has 9; 16 leaves room without
  // letting a pathological response grow the vector unbounded.
  static constexpr size_t MAX_SMART_SCOPES = 16;
  // Books per page. Kept small deliberately: the response is buffered whole and
  // ArduinoJson needs ~2-3x the JSON size to build its tree.
  static constexpr int PAGE_SIZE = 10;

 private:
  static std::string catalogBase();
  static Error httpGetJson(const std::string& url, std::string& outBody);
};
