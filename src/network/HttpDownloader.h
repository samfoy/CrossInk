#pragma once
#include <HalStorage.h>
#include <Stream.h>

#include <functional>
#include <string>
#include <utility>

/**
 * HTTP client utility for fetching content and downloading files.
 * Streams requests through esp_http_client so large downloads do not need to
 * fit in RAM.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  using CancelCallback = std::function<bool()>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  // How to authenticate the request. Basic = HTTP Basic (OPDS servers).
  // KosyncHeader = KOReader/BookOrbit style x-auth-user + x-auth-key headers,
  // where `password` is already the md5 of the real password.
  enum class AuthMode { Basic, KosyncHeader };

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  struct DownloadOptions {
    explicit DownloadOptions(bool preservePartial = false, bool resumePartial = false,
                             CancelCallback shouldCancel = nullptr, size_t bufferSize = 0,
                             AuthMode authMode = AuthMode::Basic, bool insecureTls = false)
        : preservePartial(preservePartial),
          resumePartial(resumePartial),
          shouldCancel(std::move(shouldCancel)),
          bufferSize(bufferSize),
          authMode(authMode),
          insecureTls(insecureTls) {}

    bool preservePartial;
    bool resumePartial;
    CancelCallback shouldCancel;
    size_t bufferSize;
    AuthMode authMode;
    // Skip TLS certificate verification (no CA bundle loaded). Saves ~30-40 KB
    // of heap during the handshake on the memory-starved ESP32-C3 — needed for
    // the BookOrbit catalog download, matching the kosync client's setInsecure().
    bool insecureTls;
  };

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "", AuthMode authMode = AuthMode::Basic);

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "", AuthMode authMode = AuthMode::Basic);

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "", AuthMode authMode = AuthMode::Basic);

  /**
   * Like fetchUrl(onData,...) but also reports the final HTTP status code via
   * outStatus (set to 0 if the request never got a response). Lets callers tell
   * a real 404/401 apart from a transport/heap failure.
   */
  static bool fetchUrlWithStatus(const std::string& url, const DataCallback& onData, int& outStatus,
                                 const std::string& username = "", const std::string& password = "",
                                 AuthMode authMode = AuthMode::Basic);

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      DownloadOptions options = DownloadOptions());
};
