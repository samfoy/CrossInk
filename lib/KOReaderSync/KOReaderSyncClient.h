#pragma once
#include <string>
#include <vector>

#include "KOReaderPageStatsStore.h"

/**
 * Progress data from KOReader sync server.
 */
struct KOReaderProgress {
  std::string document;  // Document hash
  std::string progress;  // XPath-like progress string
  float percentage;      // Progress percentage (0.0 to 1.0)
  std::string device;    // Device name
  std::string deviceId;  // Device ID
  int64_t timestamp;     // Unix timestamp of last update
};

/**
 * HTTP client for KOReader sync API.
 *
 * Base URL: https://sync.koreader.rocks:443/
 *
 * API Endpoints:
 *   GET  /users/auth              - Authenticate (validate credentials)
 *   GET  /syncs/progress/:document - Get progress for a document
 *   PUT  /syncs/progress          - Update progress for a document
 *   POST /plugin/page-stats       - Upload timed page-turn events (BookOrbit only;
 *                                    powers reading streak/time/pace/DNA stats)
 *
 * Authentication:
 *   x-auth-user: username
 *   x-auth-key: MD5 hash of password
 */
class KOReaderSyncClient {
 public:
  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
    NOT_FOUND,
    INVALID_AUTH_RESPONSE,
    LOW_MEMORY
  };

  /**
   * Authenticate with the sync server (validate credentials).
   * @return OK on success, error code on failure
   */
  static Error authenticate();

  /**
   * Get reading progress for a document.
   * @param documentHash The document hash (from KOReaderDocumentId)
   * @param outProgress Output: the progress data
   * @return OK on success, NOT_FOUND if no progress exists, error code on failure
   */
  static Error getProgress(const std::string& documentHash, KOReaderProgress& outProgress);

  /**
   * Update reading progress for a document.
   * @param progress The progress data to upload
   * @return OK on success, error code on failure
   */
  static Error updateProgress(const KOReaderProgress& progress);

  /**
   * Upload buffered page-turn events to a BookOrbit server's KOReader plugin
   * page-stats endpoint (POST <base>/plugin/page-stats). The server clusters
   * these raw events into timed reading sessions, which is what feeds the
   * reading-streak / reading-time / pace / reading-DNA stats. Plain KOSync
   * progress does NOT create sessions, so without this a device only moves the
   * progress bar.
   *
   * Only supported by BookOrbit-style servers; the public sync.koreader.rocks
   * has no such endpoint and returns 404 (mapped to NOT_FOUND so the caller can
   * disable stat upload gracefully).
   *
   * @param deviceModel  Human-readable device model (e.g. "Xteink X3")
   * @param store        Buffer of pending events (must have a 32-char hash)
   * @param deviceTime   Optional device-local wall clock "YYYY-MM-DD HH:MM:SS"
   *                     (empty to omit). KOReader datetimes carry no timezone.
   * @return OK on success (2xx), NOT_FOUND if the endpoint is unsupported,
   *         error code otherwise
   */
  static Error uploadPageStats(const std::string& deviceModel, const KOReaderPageStatsStore& store,
                               const std::string& deviceTime = "");

  /**
   * One highlight/note to upload. CrossInk stores highlights by page/word index,
   * not KOReader DOM xpointers, so pos0 is a synthetic CrossInk locator
   * ("/crossink/<spine>/<page>/<word>") sent with posFormat="xpointer". The text
   * + page + chapter are the meaningful, reviewable content server-side.
   */
  struct AnnotationUpload {
    std::string text;         // highlighted text (required)
    std::string note;         // optional user note
    std::string chapter;      // chapter title (optional)
    std::string pos0;         // synthetic locator (required by server)
    std::string datetime;     // "YYYY-MM-DD HH:MM:SS" device-local
    int pageno = 0;           // 0-based page
  };

  /**
   * Upload highlights/notes for one book (POST /plugin/annotations). Chunks to
   * keep each JSON body small on the C3 heap. documentHash is the 32-hex doc id.
   * @return OK on success (2xx), NOT_FOUND if unsupported, error otherwise.
   */
  static Error uploadAnnotations(const std::string& deviceModel, const std::string& documentHash,
                                 const std::vector<AnnotationUpload>& annotations, const std::string& deviceTime = "");

  /** One server annotation pushed down by the exchange endpoint. */
  struct AnnotationDownload {
    int serverId = 0;
    int version = 0;
    std::string text;
    std::string note;
    std::string chapter;
    int pageno = -1;
  };

  /**
   * Bidirectional annotation exchange for one book (POST /plugin/annotations/
   * exchange). Phase 2 is pull-oriented: we send an empty device key-set and
   * collect the server's `toApply.add` entries into `outAdds`. `outMore` is set
   * when the server has more to send (call again). documentHash is the 32-hex id.
   * @return OK on success (2xx), NOT_FOUND if unsupported, error otherwise.
   */
  static Error exchangeAnnotations(const std::string& deviceModel, const std::string& documentHash,
                                   std::vector<AnnotationDownload>& outAdds, bool& outMore,
                                   const std::string& deviceTime = "");

  /**
   * Acknowledge applied server annotations (POST /plugin/annotations/exchange-ack)
   * so the server advances its per-device sync cursor and stops re-pushing them.
   */
  static Error ackAnnotations(const std::string& deviceModel, const std::string& documentHash,
                              const std::vector<AnnotationDownload>& applied, const std::string& deviceTime = "");

  /**
   * Get human-readable error message.
   */
  static std::string errorString(Error error);

  /** HTTP status code from the last request (for diagnostics). */
  static int lastHttpCode;

  /** Transport-layer error from the last request (for diagnostics). */
  static int lastTransportError;
};
