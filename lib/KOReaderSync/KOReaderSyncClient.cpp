#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <HTTPClient.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#ifndef SIMULATOR
#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_http_client.h>
#endif

#include <HwSim.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>

#include "KOReaderCredentialStore.h"

int KOReaderSyncClient::lastHttpCode = 0;
int KOReaderSyncClient::lastTransportError = 0;

namespace {
constexpr char DEVICE_ID[] = "crossink-device";

// Client identifier sent in page-stats uploads. The server caps `pluginVersion`
// at 20 chars, so this must stay short — do NOT use the full CROSSINK_VERSION
// build string (branch builds like "1.4.0-dev+feat-bookorbit-pagestats" overflow
// and the whole upload 400s). Bump the numeric suffix when the wire format of
// the page-stats payload changes.
constexpr char PAGESTATS_PLUGIN_VERSION[] = "crossink-ps-1";
// Same 20-char cap applies; bump when the annotation payload changes.
constexpr char ANNOTATIONS_PLUGIN_VERSION[] = "crossink-an-1";

std::string formatHttpStatusMessage(int httpCode) {
  char buffer[96];
  snprintf(buffer, sizeof(buffer), tr(STR_KOREADER_SYNC_HTTP_STATUS_FORMAT), httpCode);
  return std::string(buffer);
}

std::string networkErrorMessage() {
#ifdef SIMULATOR
  switch (KOReaderSyncClient::lastTransportError) {
    case HTTPC_ERROR_CONNECTION_REFUSED:
    case HTTPC_ERROR_NOT_CONNECTED:
    case HTTPC_ERROR_NO_HTTP_SERVER:
      return tr(STR_KOREADER_SYNC_NETWORK_REFUSED);
    case HTTPC_ERROR_CONNECTION_LOST:
    case HTTPC_ERROR_READ_TIMEOUT:
      return tr(STR_KOREADER_SYNC_NETWORK_TIMEOUT);
    default:
      return tr(STR_KOREADER_SYNC_NETWORK_ERROR);
  }
#else
  switch (KOReaderSyncClient::lastTransportError) {
    case ESP_ERR_HTTP_CONNECT:
    case ESP_ERR_HTTP_CONNECTING:
    case ESP_ERR_HTTP_CONNECTION_CLOSED:
      return tr(STR_KOREADER_SYNC_NETWORK_REFUSED);
    case ESP_ERR_HTTP_FETCH_HEADER:
    case ESP_ERR_HTTP_EAGAIN:
    case ESP_ERR_HTTP_READ_TIMEOUT:
    case ESP_ERR_HTTP_INCOMPLETE_DATA:
      return tr(STR_KOREADER_SYNC_NETWORK_TIMEOUT);
    case ESP_ERR_HTTP_INVALID_TRANSPORT:
      return tr(STR_KOREADER_SYNC_NETWORK_TLS);
    default:
      return tr(STR_KOREADER_SYNC_NETWORK_ERROR);
  }
#endif
}

const char* classifyJsonBody(const char* body) {
  if (!body || body[0] == '\0') return "empty response";

  const char* cursor = body;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
    cursor++;
  }

  if (*cursor == '\0') return "blank response";
  if (*cursor == '<') return "HTML response";
  if (*cursor != '{' && *cursor != '[') return "non-JSON response";
  return "malformed JSON";
}

void logJsonParseFailure(const char* context, DeserializationError error, const char* body) {
  char preview[97];
  size_t i = 0;
  if (body) {
    for (; i < sizeof(preview) - 1 && body[i] != '\0'; i++) {
      const char c = body[i];
      preview[i] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    }
  }
  preview[i] = '\0';

  LOG_ERR("KOSync", "%s JSON parse failed: %s (%s, preview=\"%s\")", context, error.c_str(), classifyJsonBody(body),
          preview);
}

KOReaderSyncClient::Error validateAuthResponse(const char* body) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, body ? body : "");
  if (error) {
    logJsonParseFailure("Auth", error, body);
    return KOReaderSyncClient::JSON_ERROR;
  }

  if (!doc.is<JsonObject>()) {
    LOG_ERR("KOSync", "Auth response was not a JSON object");
    return KOReaderSyncClient::INVALID_AUTH_RESPONSE;
  }

  const char* authorized = doc["authorized"] | nullptr;
  if (authorized && std::strcmp(authorized, "OK") != 0) {
    LOG_ERR("KOSync", "Auth response explicitly denied authorization");
    return KOReaderSyncClient::INVALID_AUTH_RESPONSE;
  }

  return KOReaderSyncClient::OK;
}

// Cloudflare tunnels send a 3-cert Google Trust Services chain. During the TLS handshake
// mbedTLS makes many small allocations that collectively consume ~48KB of heap. With only
// ~50KB free after WiFi connects, the session drove min-free-ever down to 2600 bytes before
// failing with MBEDTLS_ERR_X509_ALLOC_FAILED (-0x2880). Check total free heap (not max
// contiguous block) because the failure mode is aggregate exhaustion, not one large alloc.
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;

// Shared by BOTH the simulator and device HTTPClient paths (the feature POST
// helpers below use HTTPClient + WiFiClientSecure::setInsecure() on device too,
// not just in the simulator, so these must be available unconditionally).
void addAuthHeaders(HTTPClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername().c_str());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password().c_str());
  http.setAuthorization(KOREADER_STORE.getUsername().c_str(), KOREADER_STORE.getPassword().c_str());
}

bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }

#ifndef SIMULATOR
// Small TLS buffers to fit in ESP32-C3's limited heap (~46KB free after WiFi).
// KOSync payloads are tiny JSON (<1KB), so 2KB buffers are sufficient.
// Default 16KB buffers cause OOM during TLS handshake.
constexpr int HTTP_BUF_SIZE = 2048;

void logHeapStats(const char* phase, const char* url = nullptr) {
  LOG_DBG("KOSync", "%s%s%s heap: free=%u min=%u max_alloc=%u", phase, url ? " " : "", url ? url : "",
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

// Response buffer for reading HTTP body
struct ResponseBuffer {
  char* data = nullptr;
  int len = 0;
  int capacity = 0;

  ~ResponseBuffer() { free(data); }

  bool ensure(int size) {
    if (size <= capacity) return true;
    char* newData = (char*)realloc(data, size);
    if (!newData) return false;
    data = newData;
    capacity = size;
    return true;
  }
};

// HTTP event handler to collect response body
esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
    if (buf->ensure(buf->len + evt->data_len + 1)) {
      memcpy(buf->data + buf->len, evt->data, evt->data_len);
      buf->len += evt->data_len;
      buf->data[buf->len] = '\0';
    } else {
      LOG_ERR("KOSync", "Response buffer allocation failed (%d bytes)", evt->data_len);
    }
  }
  return ESP_OK;
}

// Create configured esp_http_client with small TLS buffers
esp_http_client_handle_t createClient(const char* url, ResponseBuffer* buf,
                                      esp_http_client_method_t method = HTTP_METHOD_GET) {
  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = httpEventHandler;
  config.user_data = buf;
  config.method = method;
  config.timeout_ms = 15000;
  config.buffer_size = HTTP_BUF_SIZE;
  config.buffer_size_tx = HTTP_BUF_SIZE;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  // HTTP Basic Auth for Calibre-Web-Automated compatibility
  config.username = KOREADER_STORE.getUsername().c_str();
  config.password = KOREADER_STORE.getPassword().c_str();
  config.auth_type = HTTP_AUTH_TYPE_BASIC;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return nullptr;

  // KOSync auth headers
  if (esp_http_client_set_header(client, "Accept", "application/vnd.koreader.v1+json") != ESP_OK ||
      esp_http_client_set_header(client, "x-auth-user", KOREADER_STORE.getUsername().c_str()) != ESP_OK ||
      esp_http_client_set_header(client, "x-auth-key", KOREADER_STORE.getMd5Password().c_str()) != ESP_OK) {
    LOG_ERR("KOSync", "Failed to set auth headers");
    esp_http_client_cleanup(client);
    return nullptr;
  }

  return client;
}
#endif

// Performs a JSON POST to `url` with `body`, reusing the same auth + TLS setup
// as the other calls. Returns the HTTP status code (>0) or a negative transport
// error. Sets outTransportErr to the transport-layer error code. Shared by
// uploadPageStats so the chunk loop doesn't duplicate the #ifdef plumbing.
int doJsonPost(const std::string& url, const std::string& body, int& outHttpCode, int& outTransportErr) {
  outHttpCode = 0;
  outTransportErr = 0;
#ifdef SIMULATOR
  hwsim::TlsHandshakeScope tlsScope;  // model the mid-handshake heap trough
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;
  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  // The simulator's mock HTTPClient only exposes POST(const char*); the real
  // Arduino HTTPClient also has POST(uint8_t*, len). body is a std::string so
  // c_str() is safe for both (JSON has no embedded NULs).
  const int httpCode = http.POST(body.c_str());
  http.end();
  outHttpCode = httpCode;
  outTransportErr = (httpCode < 0) ? httpCode : 0;
  return httpCode;
#else
  // Device path: use HTTPClient + WiFiClientSecure::setInsecure() — the SAME
  // heap-safe TLS stack as kosync progress and the catalog client. Do NOT use
  // esp_http_client + esp_crt_bundle_attach here: the Mozilla CA bundle costs
  // ~40 KB of small allocations during the handshake, and when this POST runs
  // late in a sync (after progress + annotation upload) the heap is fragmented
  // down to ~a few hundred bytes free -> mbedTLS can't allocate for cert
  // verification -> "esp-x509-crt-bundle: PK verify failed" / handshake -0x3000
  // (an OOM masquerading as a cert error). setInsecure skips the bundle and cert
  // verification, matching the trust model kosync already uses to this server.
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;
  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  // Bound the request: on a weak/keep-alive TLS socket the ESP32 HTTPClient
  // POST-body write can STALL for minutes (observed: a 4.8 KB page-stats chunk
  // hung ~153s until Caddy 504'd, connection timed out; the body never fully
  // arrived). setTimeout caps the socket read/write so a stall aborts in ~20s
  // and the caller can retry; Connection: close + setReuse(false) forces a fresh
  // connection per POST so a half-wedged keep-alive socket can't poison the next
  // chunk. (The server accepts a 60-event chunk in <0.5s, so 20s is ample.)
  http.setTimeout(20000);
  http.setConnectTimeout(20000);
  http.setReuse(false);
  http.addHeader("Connection", "close");
  const int httpCode = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body.c_str())), body.length());
  http.end();
  outHttpCode = httpCode;
  outTransportErr = (httpCode < 0) ? httpCode : 0;
  return httpCode;
#endif
}

// Like doJsonPost but also returns the response body (needed by the annotation
// exchange, which reads the server's toApply list). Same TLS/auth plumbing.
std::string doJsonPostWithResponse(const std::string& url, const std::string& body, int& outHttpCode,
                                   int& outTransportErr) {
  outHttpCode = 0;
  outTransportErr = 0;
#ifdef SIMULATOR
  hwsim::TlsHandshakeScope tlsScope;
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;
  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.POST(body.c_str());
  std::string resp;
  if (httpCode > 0) resp = http.getString().c_str();
  http.end();
  outHttpCode = httpCode;
  outTransportErr = (httpCode < 0) ? httpCode : 0;
  return resp;
#else
  // Device path: HTTPClient + WiFiClientSecure::setInsecure() (heap-safe, no CA
  // bundle) — see the rationale in doJsonPost. This is the call the annotation
  // exchange uses; on the old esp_crt_bundle path it OOM'd at the handshake
  // ("PK verify failed", Min Free ~400 bytes) because it runs last in a sync.
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;
  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  // Bound the request (see doJsonPost): cap a stalled body write at ~20s and use
  // a fresh non-keep-alive connection so a wedged socket can't hang for minutes.
  http.setTimeout(20000);
  http.setConnectTimeout(20000);
  http.setReuse(false);
  http.addHeader("Connection", "close");
  const int httpCode = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body.c_str())), body.length());
  std::string resp;
  if (httpCode > 0) resp = http.getString().c_str();
  http.end();
  outHttpCode = httpCode;
  outTransportErr = (httpCode < 0) ? httpCode : 0;
  return resp;
#endif
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  const uint32_t freeHeap = HWSIM_FREE_HEAP();
  LOG_DBG("KOSync", "Authenticating: %s (heap: %u)", url.c_str(), (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient heap for TLS handshake: %u bytes free (need %u)", freeHeap, MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

#ifdef SIMULATOR
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);

  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;

  LOG_DBG("KOSync", "Auth response: %d", httpCode);

  if (httpCode == 200) {
    String responseBody = http.getString();
    http.end();
    return validateAuthResponse(responseBody.c_str());
  }

  http.end();

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
#else
  ResponseBuffer buf;
  logHeapStats("Before auth client", url.c_str());
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) {
    lastTransportError = ESP_ERR_NO_MEM;
    return NETWORK_ERROR;
  }

  logHeapStats("Before auth perform");
  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  lastTransportError = static_cast<int>(err);
  logHeapStats("After auth perform");
  esp_http_client_cleanup(client);

  LOG_DBG("KOSync", "Auth response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200) return validateAuthResponse(buf.data);
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
#endif
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  const uint32_t freeHeap = HWSIM_FREE_HEAP();
  LOG_DBG("KOSync", "Getting progress: %s (heap: %u)", url.c_str(), (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient heap for TLS handshake: %u bytes free (need %u)", freeHeap, MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

#ifdef SIMULATOR
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);

  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;

  if (httpCode == 200) {
    String responseBody = http.getString();
    http.end();

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, responseBody);

    if (error) {
      logJsonParseFailure("Get progress", error, responseBody.c_str());
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  http.end();
  LOG_DBG("KOSync", "Get progress response: %d", httpCode);

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
#else
  ResponseBuffer buf;
  logHeapStats("Before get client", url.c_str());
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) {
    lastTransportError = ESP_ERR_NO_MEM;
    return NETWORK_ERROR;
  }

  logHeapStats("Before get perform");
  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  lastTransportError = static_cast<int>(err);
  logHeapStats("After get perform");
  esp_http_client_cleanup(client);

  LOG_DBG("KOSync", "Get progress response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;

  if (httpCode == 200 && buf.data) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, buf.data);

    if (error) {
      logJsonParseFailure("Get progress", error, buf.data);
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  return SERVER_ERROR;
#endif
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  const uint32_t freeHeap = HWSIM_FREE_HEAP();
  LOG_DBG("KOSync", "Updating progress: %s (heap: %u)", url.c_str(), (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient heap for TLS handshake: %u bytes free (need %u)", freeHeap, MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

  // Build JSON body
  JsonDocument doc;
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = progress.device;
  doc["device_id"] = DEVICE_ID;

  std::string body;
  serializeJson(doc, body);

  LOG_DBG("KOSync", "Request body: %s", body.c_str());

#ifdef SIMULATOR
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.PUT(body.c_str());
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  http.end();

  LOG_DBG("KOSync", "Update progress response: %d", httpCode);

  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
#else
  ResponseBuffer buf;
  logHeapStats("Before put client", url.c_str());
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_PUT);
  if (!client) {
    lastTransportError = ESP_ERR_NO_MEM;
    return NETWORK_ERROR;
  }

  if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
      esp_http_client_set_post_field(client, body.c_str(), body.length()) != ESP_OK) {
    LOG_ERR("KOSync", "Failed to set request body");
    lastTransportError = ESP_ERR_INVALID_STATE;
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  LOG_DBG("KOSync", "PUT body bytes=%u", static_cast<unsigned>(body.length()));
  logHeapStats("Before put perform");
  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  lastTransportError = static_cast<int>(err);
  logHeapStats("After put perform");
  esp_http_client_cleanup(client);

  LOG_DBG("KOSync", "Update progress response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
#endif
}

KOReaderSyncClient::Error KOReaderSyncClient::uploadPageStats(const std::string& deviceModel,
                                                              const KOReaderPageStatsStore& store,
                                                              const std::string& deviceTime) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOStats", "No credentials configured");
    return NO_CREDENTIALS;
  }
  if (store.empty() || store.documentHash().size() != 32) {
    return OK;  // nothing to upload
  }

  const uint32_t freeHeap = HWSIM_FREE_HEAP();
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("KOStats", "Insufficient heap for TLS handshake: %u bytes free (need %u)", (unsigned)freeHeap,
            (unsigned)MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/plugin/page-stats";
  const auto& events = store.events();

  // Chunk events to keep each JSON body small on the ESP32-C3's tight heap AND
  // small on the wire — a large body over a weak keep-alive TLS socket can stall
  // the write for minutes (see doJsonPost). 30 events ≈ 2.5 KB, which the server
  // accepts in well under a second. The server clusters events across requests
  // by (deviceId,bookFileId,startTime), so splitting a session across chunks is
  // safe and idempotent.
  constexpr size_t CHUNK = 30;
  for (size_t start = 0; start < events.size(); start += CHUNK) {
    const size_t end = std::min(start + CHUNK, events.size());

    JsonDocument doc;
    doc["deviceId"] = DEVICE_ID;
    doc["deviceModel"] = deviceModel;
    doc["pluginVersion"] = PAGESTATS_PLUGIN_VERSION;
    if (!deviceTime.empty()) {
      doc["deviceTime"] = deviceTime;
    }
    JsonArray books = doc["books"].to<JsonArray>();
    JsonObject book = books.add<JsonObject>();
    book["hash"] = store.documentHash();
    JsonArray evs = book["events"].to<JsonArray>();
    for (size_t i = start; i < end; ++i) {
      const KOReaderPageStatEvent& e = events[i];
      JsonObject ev = evs.add<JsonObject>();
      ev["page"] = e.progressBp;                                    // basis points (0..10000)
      ev["startTime"] = e.startTime;                                // unix epoch seconds
      ev["durationSeconds"] = e.durationSeconds;                    // dwell time
      ev["totalPages"] = KOReaderPageStatsStore::PROGRESS_SCALE;    // fixed denominator
    }

    std::string body;
    serializeJson(doc, body);

    int httpCode = 0;
    int transportErr = 0;
    LOG_DBG("KOStats", "Uploading page-stats chunk %u-%u (%u bytes, heap %u)", (unsigned)start, (unsigned)end,
            (unsigned)body.length(), (unsigned)ESP.getFreeHeap());
    doJsonPost(url, body, httpCode, transportErr);
    lastHttpCode = httpCode;
    lastTransportError = transportErr;

    if (httpCode == 200 || httpCode == 201 || httpCode == 202 || httpCode == 204) {
      continue;  // chunk accepted
    }
    if (httpCode == 401) return AUTH_FAILED;
    if (httpCode == 404 || httpCode == 405 || httpCode == 501) {
      return NOT_FOUND;  // server doesn't implement/allow this endpoint (plain kosync)
    }
    if (httpCode <= 0) return NETWORK_ERROR;
    return SERVER_ERROR;
  }

  LOG_INF("KOStats", "Uploaded %u page-stat events for %s", (unsigned)events.size(), store.documentHash().c_str());
  return OK;
}

KOReaderSyncClient::Error KOReaderSyncClient::uploadAnnotations(const std::string& deviceModel,
                                                                const std::string& documentHash,
                                                                const std::vector<AnnotationUpload>& annotations,
                                                                const std::string& deviceTime) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOAnnot", "No credentials configured");
    return NO_CREDENTIALS;
  }
  if (annotations.empty() || documentHash.size() != 32) {
    return OK;  // nothing to upload
  }

  const uint32_t freeHeap = HWSIM_FREE_HEAP();
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("KOAnnot", "Insufficient heap for TLS handshake: %u bytes free (need %u)", (unsigned)freeHeap,
            (unsigned)MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/plugin/annotations";

  // Chunk annotations to keep each JSON body small on the C3's tight heap. The
  // server upserts by (hash, pos0, datetime), so re-sending is idempotent.
  constexpr size_t CHUNK = 20;
  for (size_t start = 0; start < annotations.size(); start += CHUNK) {
    const size_t end = std::min(start + CHUNK, annotations.size());

    JsonDocument doc;
    doc["deviceId"] = DEVICE_ID;
    doc["deviceModel"] = deviceModel;
    doc["pluginVersion"] = ANNOTATIONS_PLUGIN_VERSION;
    if (!deviceTime.empty()) {
      doc["deviceTime"] = deviceTime;
    }
    JsonArray books = doc["books"].to<JsonArray>();
    JsonObject book = books.add<JsonObject>();
    book["hash"] = documentHash;
    JsonArray anns = book["annotations"].to<JsonArray>();
    for (size_t i = start; i < end; ++i) {
      const AnnotationUpload& a = annotations[i];
      JsonObject an = anns.add<JsonObject>();
      an["datetime"] = a.datetime;
      an["drawer"] = "lighten";           // CrossInk has one highlight style
      an["posFormat"] = "xpointer";       // synthetic locator (see header note)
      an["pos0"] = a.pos0;
      if (a.pageno >= 0) an["pageno"] = a.pageno;
      if (!a.text.empty()) an["text"] = a.text;
      if (!a.note.empty()) an["note"] = a.note;
      if (!a.chapter.empty()) an["chapter"] = a.chapter;
    }

    std::string body;
    serializeJson(doc, body);

    int httpCode = 0;
    int transportErr = 0;
    LOG_DBG("KOAnnot", "Uploading annotations chunk %u-%u (%u bytes, heap %u)", (unsigned)start, (unsigned)end,
            (unsigned)body.length(), (unsigned)ESP.getFreeHeap());
    doJsonPost(url, body, httpCode, transportErr);
    lastHttpCode = httpCode;
    lastTransportError = transportErr;

    if (httpCode == 200 || httpCode == 201 || httpCode == 202 || httpCode == 204) {
      continue;  // chunk accepted
    }
    if (httpCode == 401) return AUTH_FAILED;
    if (httpCode == 404 || httpCode == 405 || httpCode == 501) {
      return NOT_FOUND;  // server doesn't implement this endpoint (plain kosync)
    }
    if (httpCode <= 0) return NETWORK_ERROR;
    return SERVER_ERROR;
  }

  LOG_INF("KOAnnot", "Uploaded %u annotations for %s", (unsigned)annotations.size(), documentHash.c_str());
  return OK;
}

KOReaderSyncClient::Error KOReaderSyncClient::exchangeAnnotations(const std::string& deviceModel,
                                                                  const std::string& documentHash,
                                                                  std::vector<AnnotationDownload>& outAdds,
                                                                  bool& outMore, const std::string& deviceTime) {
  lastHttpCode = 0;
  lastTransportError = 0;
  outAdds.clear();
  outMore = false;
  if (!KOREADER_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (documentHash.size() != 32) return OK;

  const uint32_t freeHeap = HWSIM_FREE_HEAP();
  if (freeHeap < MIN_HEAP_FOR_TLS) return LOW_MEMORY;

  const std::string url = KOREADER_STORE.getBaseUrl() + "/plugin/annotations/exchange";

  // Pull-oriented: send an empty device key-set (keysComplete=false disables
  // server-side deletion detection, which we can't act on yet) and no changes.
  JsonDocument doc;
  doc["deviceId"] = DEVICE_ID;
  doc["deviceModel"] = deviceModel;
  doc["pluginVersion"] = ANNOTATIONS_PLUGIN_VERSION;
  if (!deviceTime.empty()) doc["deviceTime"] = deviceTime;
  JsonArray books = doc["books"].to<JsonArray>();
  JsonObject book = books.add<JsonObject>();
  book["hash"] = documentHash;
  // Use .to<JsonArray>() to materialize REAL empty arrays as members of `book`.
  // `book["keys"] = JsonArray();` serializes as `null` in ArduinoJson v7 (a
  // detached, unbound array), which the server's DTO rejects with HTTP 400
  // ("books.0.keys must be an array" / "changes must be an array"). Pull-only:
  // empty keys + keysComplete=false disables server-side deletion detection
  // (which we can't act on yet); empty changes = no local edits to push.
  book["keys"].to<JsonArray>();          // empty []
  book["keysComplete"] = false;
  book["changes"].to<JsonArray>();       // empty []
  std::string body;
  serializeJson(doc, body);

  int httpCode = 0;
  int transportErr = 0;
  const std::string resp = doJsonPostWithResponse(url, body, httpCode, transportErr);
  lastHttpCode = httpCode;
  lastTransportError = transportErr;

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404 || httpCode == 405 || httpCode == 501) return NOT_FOUND;
  if (httpCode < 200 || httpCode >= 300) return (httpCode <= 0) ? NETWORK_ERROR : SERVER_ERROR;

  JsonDocument rdoc;
  if (deserializeJson(rdoc, resp)) return SERVER_ERROR;
  JsonArrayConst results = rdoc["results"].as<JsonArrayConst>();
  if (results.isNull()) return OK;
  for (JsonObjectConst r : results) {
    if (r["more"] | false) outMore = true;
    JsonArrayConst adds = r["toApply"]["add"].as<JsonArrayConst>();
    if (adds.isNull()) continue;
    for (JsonObjectConst a : adds) {
      AnnotationDownload d;
      d.serverId = a["serverId"] | 0;
      d.version = a["version"] | 0;
      d.text = a["text"].as<const char*>() ? a["text"].as<const char*>() : "";
      d.note = a["note"].as<const char*>() ? a["note"].as<const char*>() : "";
      d.chapter = a["chapter"].as<const char*>() ? a["chapter"].as<const char*>() : "";
      d.pageno = a["pageno"].isNull() ? -1 : (a["pageno"] | -1);
      if (d.serverId > 0) outAdds.push_back(std::move(d));
    }
  }
  LOG_INF("KOAnnot", "Exchange pulled %u server annotations for %s", (unsigned)outAdds.size(), documentHash.c_str());
  return OK;
}

KOReaderSyncClient::Error KOReaderSyncClient::ackAnnotations(const std::string& deviceModel,
                                                             const std::string& documentHash,
                                                             const std::vector<AnnotationDownload>& applied,
                                                             const std::string& deviceTime) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!KOREADER_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (documentHash.size() != 32 || applied.empty()) return OK;

  const std::string url = KOREADER_STORE.getBaseUrl() + "/plugin/annotations/exchange-ack";

  // Ack in chunks (server caps applied[] at 200 per book).
  constexpr size_t CHUNK = 200;
  for (size_t start = 0; start < applied.size(); start += CHUNK) {
    const size_t end = std::min(start + CHUNK, applied.size());
    JsonDocument doc;
    doc["deviceId"] = DEVICE_ID;
    doc["deviceModel"] = deviceModel;
    doc["pluginVersion"] = ANNOTATIONS_PLUGIN_VERSION;
    if (!deviceTime.empty()) doc["deviceTime"] = deviceTime;
    JsonArray books = doc["books"].to<JsonArray>();
    JsonObject book = books.add<JsonObject>();
    book["hash"] = documentHash;
    JsonArray app = book["applied"].to<JsonArray>();
    for (size_t i = start; i < end; ++i) {
      JsonObject e = app.add<JsonObject>();
      e["serverId"] = applied[i].serverId;
      e["version"] = applied[i].version;
      e["status"] = "applied";
    }
    book["deleted"].to<JsonArray>();  // empty [] (none; = JsonArray() serializes as null -> HTTP 400)

    std::string body;
    serializeJson(doc, body);
    int httpCode = 0;
    int transportErr = 0;
    doJsonPost(url, body, httpCode, transportErr);
    lastHttpCode = httpCode;
    lastTransportError = transportErr;
    if (httpCode == 401) return AUTH_FAILED;
    if (httpCode == 404 || httpCode == 405 || httpCode == 501) return NOT_FOUND;
    if (httpCode < 200 || httpCode >= 300) return (httpCode <= 0) ? NETWORK_ERROR : SERVER_ERROR;
  }
  return OK;
}

std::string KOReaderSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return tr(STR_NO_CREDENTIALS_MSG);
    case NETWORK_ERROR:
      return networkErrorMessage();
    case AUTH_FAILED:
      return tr(STR_KOREADER_SYNC_AUTH_REJECTED);
    case SERVER_ERROR:
      if (lastHttpCode == 404) return tr(STR_KOREADER_SYNC_HTTP_404);
      if (lastHttpCode > 0) return formatHttpStatusMessage(lastHttpCode);
      return tr(STR_KOREADER_SYNC_SERVER_ERROR);
    case JSON_ERROR:
      return tr(STR_KOREADER_SYNC_BAD_RESPONSE);
    case NOT_FOUND:
      return tr(STR_NO_REMOTE_MSG);
    case INVALID_AUTH_RESPONSE:
      return tr(STR_KOREADER_SYNC_BAD_RESPONSE);
    case LOW_MEMORY:
      return tr(STR_KOREADER_SYNC_LOW_MEMORY);
    default:
      return tr(STR_UNKNOWN_ERROR);
  }
}
