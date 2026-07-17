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
   * Get human-readable error message.
   */
  static std::string errorString(Error error);

  /** HTTP status code from the last request (for diagnostics). */
  static int lastHttpCode;

  /** Transport-layer error from the last request (for diagnostics). */
  static int lastTransportError;
};
