#include "KOReaderCatalogClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "KOReaderCredentialStore.h"
#include "network/HttpDownloader.h"

namespace {

// The catalog lives under the kosync base URL (which already ends in
// /api/v1/koreader), e.g. https://samfp.tech/books/api/v1/koreader/plugin/catalog.
std::string joinFirstAuthor(JsonArrayConst authors) {
  if (authors.isNull() || authors.size() == 0) return "";
  return std::string(authors[0].as<const char*>() ? authors[0].as<const char*>() : "");
}

float progressOrUnknown(JsonVariantConst v) {
  if (v.isNull()) return -1.0f;
  return v.as<float>();
}

// Populate a list item from a catalog book JSON object (shared by list +
// dashboard, which use the same item shape).
void parseItem(JsonObjectConst o, BookOrbitCatalogItem& it) {
  it.id = o["id"] | 0;
  it.title = o["title"].as<const char*>() ? o["title"].as<const char*>() : "";
  it.author = joinFirstAuthor(o["authors"]);
  it.seriesName = o["seriesName"].as<const char*>() ? o["seriesName"].as<const char*>() : "";
  it.readStatus = o["readStatus"].as<const char*>() ? o["readStatus"].as<const char*>() : "";
  it.progressPercentage = progressOrUnknown(o["progressPercentage"]);
  it.hasCover = o["hasCover"] | false;
  // list/dashboard don't include files; primaryFileId resolved on detail fetch
  it.primaryFileId = 0;
  JsonArrayConst formats = o["formats"];
  if (!formats.isNull() && formats.size() > 0) {
    it.primaryFormat = formats[0].as<const char*>() ? formats[0].as<const char*>() : "";
  }
}

}  // namespace

std::string KOReaderCatalogClient::catalogBase() {
  return KOREADER_STORE.getBaseUrl() + "/plugin/catalog";
}

std::string KOReaderCatalogClient::downloadUrl(int fileId) {
  return catalogBase() + "/files/" + std::to_string(fileId) + "/download";
}

std::string KOReaderCatalogClient::thumbnailUrl(int bookId) {
  return catalogBase() + "/books/" + std::to_string(bookId) + "/thumbnail";
}

KOReaderCatalogClient::Error KOReaderCatalogClient::httpGetJson(const std::string& url, std::string& outBody) {
  if (!KOREADER_STORE.hasCredentials()) return NO_CREDENTIALS;
  outBody.clear();

  LOG_DBG("BOCAT", "GET %s (freeHeap=%u)", url.c_str(), ESP.getFreeHeap());

  // Use the same TLS path as the working kosync client: WiFiClientSecure with
  // setInsecure() (skip cert verification). This deliberately avoids
  // esp_crt_bundle_attach, which loads the whole Mozilla CA bundle into RAM and
  // pushes the C3 into OOM during the handshake (~5 KB free at the peak). The
  // kosync PUT/page-stats already talk to this same server this way.
  const bool https = url.rfind("https://", 0) == 0;
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;
#ifdef SIMULATOR
  // Mock HTTPClient::begin() returns void.
  if (https) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
#else
  bool begun = false;
  if (https) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    begun = http.begin(*secureClient, url.c_str());
  } else {
    begun = http.begin(plainClient, url.c_str());
  }
  if (!begun) {
    LOG_ERR("BOCAT", "http.begin failed");
    return NETWORK_ERROR;
  }
#endif

  http.addHeader("x-auth-user", KOREADER_STORE.getUsername().c_str());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password().c_str());
  http.addHeader("Accept", "application/json");

  const int status = http.GET();
  if (status == 200) {
    outBody = http.getString().c_str();
  }
  http.end();

  LOG_DBG("BOCAT", "GET done: status=%d bytes=%u freeHeap=%u", status,
          static_cast<unsigned>(outBody.size()), ESP.getFreeHeap());

  if (status == 200) return OK;
  if (status == 404 || status == 405 || status == 501) return UNAVAILABLE;
  if (status == 401 || status == 403) return NO_CREDENTIALS;
  return NETWORK_ERROR;  // <0 = transport/TLS/heap; 5xx = server
}

KOReaderCatalogClient::Error KOReaderCatalogClient::fetchDashboard(BookOrbitDashboard& stats,
                                                                   std::vector<BookOrbitCatalogItem>& continueReading) {
  stats = BookOrbitDashboard{};
  continueReading.clear();

  std::string body;
  const Error e = httpGetJson(catalogBase() + "/dashboard", body);
  if (e != OK) return e;

  JsonDocument doc;
  const DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) return PARSE_ERROR;

  // Summary stats (all optional; leave as -1/unknown when absent).
  const char* dn = doc["displayName"].as<const char*>();
  if (dn) stats.displayName = dn;
  stats.totalBooks = doc["totalBooks"] | -1;

  JsonObjectConst streak = doc["readingStreak"].as<JsonObjectConst>();
  if (!streak.isNull()) {
    stats.currentStreak = streak["currentStreak"] | -1;
    stats.longestStreak = streak["longestStreak"] | -1;
  }
  JsonObjectConst goal = doc["readingGoal"].as<JsonObjectConst>();
  if (!goal.isNull()) {
    stats.goalBooks = goal["goalBooks"] | -1;
    stats.goalCompleted = goal["completedBooks"] | -1;
    stats.goalYear = goal["year"] | 0;
  }

  JsonArrayConst cr = doc["continueReading"].as<JsonArrayConst>();
  if (!cr.isNull()) {
    for (JsonObjectConst o : cr) {
      BookOrbitCatalogItem it;
      parseItem(o, it);
      if (it.id > 0) continueReading.push_back(it);
    }
  }
  return OK;
}

KOReaderCatalogClient::Error KOReaderCatalogClient::fetchContinueReading(std::vector<BookOrbitCatalogItem>& out) {
  BookOrbitDashboard ignoredStats;
  return fetchDashboard(ignoredStats, out);
}

KOReaderCatalogClient::Error KOReaderCatalogClient::fetchBooks(const std::string& sort, int page,
                                                               BookOrbitCatalogPage& out) {
  out.items.clear();
  out.page = page;
  out.total = 0;
  out.hasNext = false;

  // size=10 keeps the JSON small enough to parse on the C3's tight heap
  // (a full page of 20 is ~8.6 KB; ArduinoJson needs ~2-3x that to parse).
  std::string url = catalogBase() + "/books?sort=" + sort + "&page=" + std::to_string(page) + "&size=10";
  std::string body;
  const Error e = httpGetJson(url, body);
  if (e != OK) return e;

  JsonDocument doc;
  const DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) return PARSE_ERROR;

  out.total = doc["total"] | 0;
  out.page = doc["page"] | page;
  out.hasNext = doc["hasNext"] | false;
  JsonArrayConst items = doc["items"].as<JsonArrayConst>();
  if (!items.isNull()) {
    for (JsonObjectConst o : items) {
      BookOrbitCatalogItem it;
      parseItem(o, it);
      if (it.id > 0) out.items.push_back(it);
    }
  }
  return OK;
}

KOReaderCatalogClient::Error KOReaderCatalogClient::fetchDetail(int bookId, BookOrbitCatalogDetail& out) {
  out = BookOrbitCatalogDetail{};
  std::string url = catalogBase() + "/books/" + std::to_string(bookId);
  std::string body;
  const Error e = httpGetJson(url, body);
  if (e != OK) return e;

  JsonDocument doc;
  const DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) return PARSE_ERROR;

  out.id = doc["id"] | 0;
  out.title = doc["title"].as<const char*>() ? doc["title"].as<const char*>() : "";
  out.author = joinFirstAuthor(doc["authors"]);
  out.seriesName = doc["seriesName"].as<const char*>() ? doc["seriesName"].as<const char*>() : "";
  out.seriesIndex = doc["seriesIndex"] | -1;
  out.readStatus = doc["readStatus"].as<const char*>() ? doc["readStatus"].as<const char*>() : "";
  out.progressPercentage = progressOrUnknown(doc["progressPercentage"]);
  const char* desc = doc["description"].as<const char*>();
  if (desc) {
    out.description = desc;
    if (out.description.size() > 400) out.description.resize(400);
  }

  // Pick the primary (role=primary) EPUB file for download; fall back to the
  // first epub, then the first file.
  JsonArrayConst files = doc["files"].as<JsonArrayConst>();
  if (!files.isNull()) {
    int firstEpubId = 0;
    std::string firstEpubFmt;
    long firstEpubSize = 0;
    for (JsonObjectConst f : files) {
      const char* fmt = f["format"].as<const char*>();
      const char* role = f["role"].as<const char*>();
      const int fid = f["id"] | 0;
      const long size = f["sizeBytes"] | 0L;
      const bool isEpub = fmt && std::string(fmt) == "epub";
      const bool isPrimary = role && std::string(role) == "primary";
      if (isEpub && isPrimary) {
        out.primaryFileId = fid;
        out.primaryFormat = fmt;
        out.primarySizeBytes = size;
        break;
      }
      if (isEpub && firstEpubId == 0) {
        firstEpubId = fid;
        firstEpubFmt = fmt ? fmt : "";
        firstEpubSize = size;
      }
    }
    if (out.primaryFileId == 0 && firstEpubId != 0) {
      out.primaryFileId = firstEpubId;
      out.primaryFormat = firstEpubFmt;
      out.primarySizeBytes = firstEpubSize;
    }
  }
  return OK;
}

KOReaderCatalogClient::Error KOReaderCatalogClient::setReadStatus(int bookId, const std::string& status) {
  if (!KOREADER_STORE.hasCredentials()) return NO_CREDENTIALS;

  const std::string url = catalogBase() + "/books/" + std::to_string(bookId) + "/read-status";
  std::string bodyJson = std::string("{\"status\":\"") + status + "\"}";

  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;
  const bool https = url.rfind("https://", 0) == 0;
  if (https) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername().c_str());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password().c_str());
  http.addHeader("Content-Type", "application/json");
#ifdef SIMULATOR
  // Mock HTTPClient only exposes PUT(const char*).
  const int code = http.PUT(bodyJson.c_str());
#else
  const int code = http.PUT(reinterpret_cast<uint8_t*>(&bodyJson[0]), bodyJson.length());
#endif
  http.end();

  if (code == 200 || code == 201 || code == 204) return OK;
  if (code == 404 || code == 405) return UNAVAILABLE;
  if (code < 0) return NETWORK_ERROR;
  return NETWORK_ERROR;
}

KOReaderCatalogClient::Error KOReaderCatalogClient::downloadFile(int fileId, const std::string& destPath,
                                                                 void (*onProgress)(size_t, size_t, void*),
                                                                 void* progressCtx, const bool* cancelFlag,
                                                                 long knownTotal) {
  if (!KOREADER_STORE.hasCredentials()) return NO_CREDENTIALS;

  const std::string url = downloadUrl(fileId);
  LOG_DBG("BOCAT", "download fileId=%d -> %s (freeHeap=%u)", fileId, destPath.c_str(), ESP.getFreeHeap());

  const bool https = url.rfind("https://", 0) == 0;
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;
  if (https) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();  // no CA bundle: same as kosync, fits C3 heap
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername().c_str());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password().c_str());

  const int code = http.GET();
  if (code != 200) {
    http.end();
    LOG_ERR("BOCAT", "download HTTP %d", code);
    if (code == 404 || code == 405) return UNAVAILABLE;
    if (code == 401 || code == 403) return NO_CREDENTIALS;
    return NETWORK_ERROR;
  }

  const int httpTotal = http.getSize();  // -1 when the server streams w/o Content-Length
  // Loop/termination is driven ONLY by httpTotal (authoritative). knownTotal is a
  // best-effort figure from book detail used purely for the progress display, so a
  // stale size can't truncate or hang the actual download.
  const size_t progressTotal = httpTotal > 0 ? static_cast<size_t>(httpTotal)
                                             : (knownTotal > 0 ? static_cast<size_t>(knownTotal) : 0);

  // Make sure the destination directory exists (e.g. /books) — openFileForWrite
  // does not create parent dirs, and a fresh SD card may not have /books yet.
  const size_t slash = destPath.rfind('/');
  if (slash != std::string::npos && slash > 0) {
    const std::string dir = destPath.substr(0, slash);
    Storage.ensureDirectoryExists(dir.c_str());
  }

  HalFile file;
  if (!Storage.openFileForWrite("BOCAT", destPath, file)) {
    http.end();
    LOG_ERR("BOCAT", "cannot open %s for write", destPath.c_str());
    return NETWORK_ERROR;
  }

  Error result = OK;
#ifndef SIMULATOR
  NetworkClient* stream = http.getStreamPtr();
  if (!stream) {
    result = NETWORK_ERROR;
  } else {
    // Stream to SD in small chunks — an 8 KB stack/heap buffer keeps peak memory
    // low (the body never fully resides in RAM).
    static constexpr size_t CHUNK = 4096;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[CHUNK]);
    if (!buf) {
      result = NETWORK_ERROR;
    } else {
      size_t downloaded = 0;
      uint32_t lastData = millis();
      while (http.connected() && (httpTotal < 0 || downloaded < static_cast<size_t>(httpTotal))) {
        if (cancelFlag && *cancelFlag) {
          result = NETWORK_ERROR;
          break;
        }
        const size_t avail = stream->available();
        if (avail == 0) {
          if (millis() - lastData > 30000) {  // 30s idle timeout
            LOG_ERR("BOCAT", "download stalled at %u bytes", static_cast<unsigned>(downloaded));
            result = NETWORK_ERROR;
            break;
          }
          delay(5);
          continue;
        }
        const size_t toRead = avail < CHUNK ? avail : CHUNK;
        const int n = stream->readBytes(buf.get(), toRead);
        if (n <= 0) {
          delay(5);
          continue;
        }
        if (file.write(buf.get(), static_cast<size_t>(n)) != static_cast<size_t>(n)) {
          LOG_ERR("BOCAT", "SD write failed at %u bytes", static_cast<unsigned>(downloaded));
          result = NETWORK_ERROR;
          break;
        }
        downloaded += static_cast<size_t>(n);
        lastData = millis();
        // Report the larger of progressTotal / downloaded so a slightly-off
        // knownTotal never shows >100% or a shrinking bar.
        const size_t shownTotal = downloaded > progressTotal ? downloaded : progressTotal;
        if (onProgress) onProgress(downloaded, shownTotal, progressCtx);
      }
      if (result == OK && httpTotal > 0 && downloaded < static_cast<size_t>(httpTotal)) {
        result = NETWORK_ERROR;  // truncated
      }
      LOG_DBG("BOCAT", "download done: %u bytes (httpTotal=%d) result=%d freeHeap=%u",
              static_cast<unsigned>(downloaded), httpTotal, static_cast<int>(result), ESP.getFreeHeap());
    }
  }
#else
  // Simulator: mock HTTPClient has no getStreamPtr; just write the buffered body.
  {
    const String body = http.getString();
    if (body.length() > 0) file.write(reinterpret_cast<const uint8_t*>(body.c_str()), body.length());
    if (onProgress) onProgress(body.length(), body.length(), progressCtx);
  }
#endif

  file.close();
  http.end();

  if (result != OK) {
    Storage.remove(destPath.c_str());  // don't leave a partial/corrupt file
  }
  return result;
}

const char* KOReaderCatalogClient::errorString(Error e) {
  switch (e) {
    case OK:
      return "OK";
    case NO_CREDENTIALS:
      return "No credentials";
    case NETWORK_ERROR:
      return "Network error";
    case UNAVAILABLE:
      return "Catalog unavailable";
    case PARSE_ERROR:
      return "Parse error";
  }
  return "Unknown";
}
