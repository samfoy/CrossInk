#include "KOReaderCatalogClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <base64.h>

#include "KOReaderCredentialStore.h"

namespace {

// Same trust model as the KOSync client: skip certificate verification. This is
// deliberate and NOT a new exposure — it is the same credentials to the same
// server over the same transport the progress sync already uses. Proper
// hardening later means pinning the server's root CA for both at once, not
// tightening one call site in isolation.
constexpr uint32_t HTTP_TIMEOUT_MS = 20000;

// Free-heap floor before a TLS handshake. The S3 has far more headroom than the
// C3 (8 MB PSRAM, ~320 KB internal SRAM), but a handshake still needs a
// contiguous block, so keep a conservative guard rather than none.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

// Chunk size for streaming a download to SD.
constexpr size_t DOWNLOAD_FLUSH_BYTES = 8192;

void applyAuthHeaders(freeink::SecureHttpClient& http) {
  http.addHeader("Accept", "application/json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  const std::string credentials = KOREADER_STORE.getUsername() + ":" + KOREADER_STORE.getPassword();
  const String encoded = base64::encode(credentials.c_str());
  http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
}

bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_FOR_TLS || maxAllocHeap < MIN_BLOCK_FOR_TLS) {
    LOG_ERR("BOCAT", "Insufficient heap for TLS: %u free (need %u), %u max block (need %u)", freeHeap,
            MIN_FREE_FOR_TLS, maxAllocHeap, MIN_BLOCK_FOR_TLS);
    return true;
  }
  return false;
}

// Map an HTTP status (or negative transport code) onto our error enum. Keeping
// the distinct causes separate matters: collapsing everything into one
// "unavailable" string is what made the C3's catalog bugs so slow to diagnose.
KOReaderCatalogClient::Error classify(int status) {
  if (status >= 200 && status < 300) return KOReaderCatalogClient::Error::OK;
  if (status == 404 || status == 405 || status == 501) return KOReaderCatalogClient::Error::UNAVAILABLE;
  if (status == 401 || status == 403) return KOReaderCatalogClient::Error::AUTH_FAILED;
  return KOReaderCatalogClient::Error::NETWORK;  // includes status <= 0 (never connected)
}

// Percent-encode a query-parameter value. string_view::data() is not
// null-terminated, so this takes a std::string deliberately.
std::string urlEncode(const std::string& in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (const unsigned char c : in) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else if (c == ' ') {
      out.push_back('+');
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

// Read one book row shared by the dashboard, listing and detail shapes.
void readItem(const JsonObjectConst& o, BookOrbitCatalogItem& item) {
  item.id = o["id"] | 0;
  item.title = o["title"] | "";
  // authors is an array of {name} or of strings depending on the endpoint.
  if (const JsonArrayConst authors = o["authors"]; !authors.isNull() && authors.size() > 0) {
    const JsonVariantConst first = authors[0];
    if (first.is<JsonObjectConst>()) {
      item.author = first["name"] | "";
    } else {
      item.author = first.as<const char*>() ? first.as<const char*>() : "";
    }
  }
  item.seriesName = o["seriesName"] | "";
  item.readStatus = o["readStatus"] | "";
  item.progressPercentage = o["progressPercentage"] | -1.0f;
  item.hasCover = o["hasCover"] | false;
}

}  // namespace

std::string KOReaderCatalogClient::catalogBase() { return KOREADER_STORE.getBaseUrl() + "/plugin/catalog"; }

std::string KOReaderCatalogClient::thumbnailUrl(int bookId) {
  return catalogBase() + "/books/" + std::to_string(bookId) + "/thumbnail";
}

const char* KOReaderCatalogClient::errorString(Error e) {
  switch (e) {
    case Error::OK:
      return "ok";
    case Error::NO_CREDENTIALS:
      return "no credentials";
    case Error::UNAVAILABLE:
      return "server has no catalog";
    case Error::AUTH_FAILED:
      return "auth failed";
    case Error::NETWORK:
      return "network error";
    case Error::PARSE:
      return "bad response";
    case Error::LOW_MEMORY:
      return "low memory";
    case Error::IO:
      return "sd write failed";
    case Error::CANCELLED:
      return "cancelled";
  }
  return "unknown";
}

KOReaderCatalogClient::Error KOReaderCatalogClient::httpGetJson(const std::string& url, std::string& outBody) {
  if (!KOREADER_STORE.hasCredentials()) return Error::NO_CREDENTIALS;
  if (insufficientHeap()) return Error::LOW_MEMORY;

  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  if (!http.begin(url)) {
    LOG_ERR("BOCAT", "Bad URL: %s", url.c_str());
    return Error::NETWORK;
  }
  applyAuthHeaders(http);

  const int status = http.GET();
  const Error e = classify(status);
  if (e != Error::OK) {
    LOG_ERR("BOCAT", "GET %s -> %d (%s), heap %u", url.c_str(), status, errorString(e), ESP.getFreeHeap());
    http.end();
    return e;
  }
  outBody = http.getString();
  http.end();
  return Error::OK;
}

KOReaderCatalogClient::Error KOReaderCatalogClient::fetchDashboard(BookOrbitDashboard& stats,
                                                                  std::vector<BookOrbitCatalogItem>& continueReading) {
  std::string body;
  const Error e = httpGetJson(catalogBase() + "/dashboard", body);
  if (e != Error::OK) return e;

  // Filter the tree: the dashboard also carries `discover` (12 books) and
  // `sections` (8) we never show, and parsing those would cost heap for nothing.
  JsonDocument filter;
  filter["username"] = true;
  filter["displayName"] = true;
  filter["totalBooks"] = true;
  filter["browseCounts"] = true;
  // A filter object under an array key applies to every element of that array.
  const JsonObject crFilter = filter["continueReading"].add<JsonObject>();
  crFilter["id"] = true;
  crFilter["title"] = true;
  crFilter["authors"] = true;
  crFilter["seriesName"] = true;
  crFilter["readStatus"] = true;
  crFilter["progressPercentage"] = true;
  crFilter["hasCover"] = true;

  JsonDocument doc;
  const DeserializationError jerr = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (jerr) {
    LOG_ERR("BOCAT", "dashboard parse: %s", jerr.c_str());
    return Error::PARSE;
  }

  stats.displayName = doc["displayName"] | doc["username"] | "";
  stats.totalBooks = doc["totalBooks"] | -1;
  if (const JsonObjectConst counts = doc["browseCounts"]; !counts.isNull()) {
    stats.inProgress = counts["inProgress"] | -1;
    stats.authors = counts["authors"] | -1;
    stats.series = counts["series"] | -1;
    stats.collections = counts["collections"] | -1;
    stats.smartScopes = counts["smartScopes"] | -1;
  }
  // Streak/goal are intentionally left at -1: BookOrbit v2.4.0 stopped sending
  // them to kosync clients. Callers must render unknown values as absent.

  continueReading.clear();
  const JsonArrayConst cr = doc["continueReading"];
  if (!cr.isNull()) {
    continueReading.reserve(cr.size());
    for (const JsonObjectConst o : cr) {
      BookOrbitCatalogItem item;
      readItem(o, item);
      if (item.id > 0) continueReading.push_back(std::move(item));
    }
  }
  return Error::OK;
}

KOReaderCatalogClient::Error KOReaderCatalogClient::fetchSmartScopes(std::vector<BookOrbitSmartScope>& out) {
  std::string body;
  const Error e = httpGetJson(catalogBase() + "/sections/smart-scopes", body);
  if (e != Error::OK) return e;

  JsonDocument doc;
  const DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) {
    LOG_ERR("BOCAT", "smart-scopes parse: %s", jerr.c_str());
    return Error::PARSE;
  }

  out.clear();
  const JsonArrayConst items = doc["items"];
  if (items.isNull()) return Error::PARSE;
  out.reserve(items.size() < MAX_SMART_SCOPES ? items.size() : MAX_SMART_SCOPES);
  for (const JsonObjectConst o : items) {
    if (out.size() >= MAX_SMART_SCOPES) break;
    BookOrbitSmartScope scope;
    // NOTE: the server sends `id` as a JSON STRING ("3"), not a number, so
    // `| 0` would silently yield 0 for every scope. Parse it explicitly.
    if (const char* idStr = o["id"]; idStr != nullptr) {
      scope.id = atoi(idStr);
    } else {
      scope.id = o["id"] | 0;
    }
    scope.title = o["title"] | "";
    if (scope.id > 0 && !scope.title.empty()) out.push_back(std::move(scope));
  }
  LOG_INF("BOCAT", "fetched %u smart scopes", static_cast<unsigned>(out.size()));
  return Error::OK;
}

KOReaderCatalogClient::Error KOReaderCatalogClient::fetchBooks(const BookOrbitBooksQuery& query,
                                                              BookOrbitCatalogPage& out) {
  std::string url = catalogBase() + "/books?size=" + std::to_string(PAGE_SIZE) + "&page=" + std::to_string(query.page);
  if (!query.sort.empty()) url += "&sort=" + query.sort;
  if (!query.search.empty()) url += "&q=" + urlEncode(query.search);
  if (!query.readStatus.empty()) url += "&readStatus=" + query.readStatus;
  if (query.seriesId > 0) url += "&seriesId=" + std::to_string(query.seriesId);
  // A saved SmartScope: this is what turns "Want to Read" into a to-be-read
  // queue without any server-side change.
  if (query.smartScopeId > 0) url += "&smartScopeId=" + std::to_string(query.smartScopeId);
  LOG_INF("BOCAT", "books url: %s", url.c_str());

  std::string body;
  const Error e = httpGetJson(url, body);
  if (e != Error::OK) return e;

  JsonDocument filter;
  filter["page"] = true;
  filter["total"] = true;
  filter["hasNext"] = true;
  const JsonObject itemFilter = filter["items"].add<JsonObject>();
  itemFilter["id"] = true;
  itemFilter["title"] = true;
  itemFilter["authors"] = true;
  itemFilter["seriesName"] = true;
  itemFilter["readStatus"] = true;
  itemFilter["progressPercentage"] = true;
  itemFilter["hasCover"] = true;

  JsonDocument doc;
  const DeserializationError jerr = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (jerr) {
    LOG_ERR("BOCAT", "books parse: %s", jerr.c_str());
    return Error::PARSE;
  }

  out.items.clear();
  out.page = doc["page"] | query.page;
  out.total = doc["total"] | 0;
  out.hasNext = doc["hasNext"] | false;
  const JsonArrayConst items = doc["items"];
  if (!items.isNull()) {
    out.items.reserve(items.size());
    for (const JsonObjectConst o : items) {
      BookOrbitCatalogItem item;
      readItem(o, item);
      if (item.id > 0) out.items.push_back(std::move(item));
    }
  }
  return Error::OK;
}

KOReaderCatalogClient::Error KOReaderCatalogClient::fetchDetail(int bookId, BookOrbitCatalogDetail& out) {
  std::string body;
  const Error e = httpGetJson(catalogBase() + "/books/" + std::to_string(bookId), body);
  if (e != Error::OK) return e;

  JsonDocument doc;
  const DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) {
    LOG_ERR("BOCAT", "detail parse: %s", jerr.c_str());
    return Error::PARSE;
  }

  out.id = doc["id"] | 0;
  out.title = doc["title"] | "";
  if (const JsonArrayConst authors = doc["authors"]; !authors.isNull() && authors.size() > 0) {
    const JsonVariantConst first = authors[0];
    out.author = first.is<JsonObjectConst>() ? (first["name"] | "")
                                             : (first.as<const char*>() ? first.as<const char*>() : "");
  }
  out.seriesName = doc["seriesName"] | "";
  out.seriesIndex = doc["seriesIndex"] | -1;
  out.seriesId = doc["seriesId"] | 0;
  out.readStatus = doc["readStatus"] | "";
  out.progressPercentage = doc["progressPercentage"] | -1.0f;
  out.hasCover = doc["hasCover"] | false;

  // Pick the EPUB the device can actually open. Prefer role=primary; fall back
  // to the first epub-format file.
  const JsonArrayConst files = doc["files"];
  if (!files.isNull()) {
    for (const JsonObjectConst f : files) {
      const std::string format = f["format"] | "";
      const std::string role = f["role"] | "";
      const bool isEpub = format == "epub" || format == "EPUB";
      if (!isEpub) continue;
      if (out.primaryFileId == 0 || role == "primary") {
        out.primaryFileId = f["id"] | 0;
        out.primaryFormat = format;
        out.primarySizeBytes = f["sizeBytes"] | 0;
        if (role == "primary") break;
      }
    }
  }
  return Error::OK;
}

KOReaderCatalogClient::Error KOReaderCatalogClient::setReadStatus(int bookId, const std::string& status) {
  if (!KOREADER_STORE.hasCredentials()) return Error::NO_CREDENTIALS;
  if (insufficientHeap()) return Error::LOW_MEMORY;

  const std::string url = catalogBase() + "/books/" + std::to_string(bookId) + "/read-status";
  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  if (!http.begin(url)) return Error::NETWORK;
  applyAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["status"] = status;
  std::string payload;
  serializeJson(doc, payload);

  // SecureHttpClient has no PUT() helper, but exposes a generic verb form.
  const int code = http.sendRequest("PUT", payload);
  http.end();
  const Error e = classify(code);
  if (e != Error::OK) LOG_ERR("BOCAT", "read-status -> %d (%s)", code, errorString(e));
  return e;
}

KOReaderCatalogClient::Error KOReaderCatalogClient::downloadFile(int fileId, const std::string& destPath,
                                                                 void (*onProgress)(size_t, size_t, void*),
                                                                 void* progressCtx, const bool* cancelFlag,
                                                                 size_t knownTotalBytes) {
  if (!KOREADER_STORE.hasCredentials()) return Error::NO_CREDENTIALS;
  if (insufficientHeap()) return Error::LOW_MEMORY;

  const std::string url = catalogBase() + "/files/" + std::to_string(fileId) + "/download";

  HalFile file;
  if (!Storage.openFileForWrite("BOCAT", destPath, file)) {
    LOG_ERR("BOCAT", "cannot open %s for write", destPath.c_str());
    return Error::IO;
  }

  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  if (!http.begin(url)) {
    file.close();
    Storage.remove(destPath.c_str());
    return Error::NETWORK;
  }
  applyAuthHeaders(http);

  size_t written = 0;
  size_t sinceFlush = 0;
  bool writeFailed = false;

  // Stream the body straight to SD. SecureHttpClient's chunked-transfer decoding
  // happens before this callback, so we never see chunk-size framing bytes --
  // that was a real corruption bug on the C3's hand-rolled socket reads.
  // No repaint happens here: the caller only updates counters, because letting a
  // render run concurrently with the transfer's allocations is what OOM'd the C3.
  const auto onData = [&](const uint8_t* data, size_t len) -> bool {
    if (writeFailed) return false;
    if (file.write(data, len) != len) {
      writeFailed = true;
      return false;
    }
    written += len;
    sinceFlush += len;
    if (sinceFlush >= DOWNLOAD_FLUSH_BYTES) {
      file.flush();
      sinceFlush = 0;
    }
    if (onProgress) onProgress(written, knownTotalBytes, progressCtx);
    return true;
  };
  const auto shouldAbort = [&]() -> bool { return cancelFlag != nullptr && *cancelFlag; };

  const int status = http.GET(onData, shouldAbort);
  http.end();
  file.close();

  const bool cancelled = shouldAbort();
  if (writeFailed || cancelled || classify(status) != Error::OK) {
    // Never leave a truncated EPUB behind: a partial file would look openable
    // and then fail to parse.
    Storage.remove(destPath.c_str());
    if (cancelled) return Error::CANCELLED;
    if (writeFailed) return Error::IO;
    const Error e = classify(status);
    LOG_ERR("BOCAT", "download -> %d (%s) after %u bytes", status, errorString(e), static_cast<unsigned>(written));
    return e;
  }

  LOG_INF("BOCAT", "downloaded %u bytes -> %s", static_cast<unsigned>(written), destPath.c_str());
  return Error::OK;
}
