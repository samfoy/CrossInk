#include "KOReaderCatalogClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
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
  const bool ok = HttpDownloader::fetchUrl(url, outBody, KOREADER_STORE.getUsername(),
                                           KOREADER_STORE.getMd5Password(),
                                           HttpDownloader::AuthMode::KosyncHeader);
  if (!ok) {
    // fetchUrl collapses non-200 (incl. 404 for plain kosync) into a failure.
    // We can't see the code here, so classify empty body as unavailable/network.
    return outBody.empty() ? UNAVAILABLE : NETWORK_ERROR;
  }
  return OK;
}

KOReaderCatalogClient::Error KOReaderCatalogClient::fetchContinueReading(std::vector<BookOrbitCatalogItem>& out) {
  out.clear();
  std::string body;
  const Error e = httpGetJson(catalogBase() + "/dashboard", body);
  if (e != OK) return e;

  // Dashboard carries several sections; we only want continueReading. Filter the
  // JSON to that array before parsing to keep the document small for the C3 heap.
  JsonDocument doc;
  const DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) return PARSE_ERROR;

  JsonArrayConst cr = doc["continueReading"].as<JsonArrayConst>();
  if (cr.isNull()) return OK;  // no in-progress books is not an error
  for (JsonObjectConst o : cr) {
    BookOrbitCatalogItem it;
    parseItem(o, it);
    if (it.id > 0) out.push_back(it);
  }
  return OK;
}

KOReaderCatalogClient::Error KOReaderCatalogClient::fetchBooks(const std::string& sort, int page,
                                                               BookOrbitCatalogPage& out) {
  out.items.clear();
  out.page = page;
  out.total = 0;
  out.hasNext = false;

  std::string url = catalogBase() + "/books?sort=" + sort + "&page=" + std::to_string(page);
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
