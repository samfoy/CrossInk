#include "TranslateClient.h"

#include <ArduinoJson.h>
#include <I18n.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include "TranslateCredentialStore.h"

namespace TranslateClient {
namespace {

// Generous: the bridge calls a large model, and a cold connection over a weak
// signal is slower than the ~1.5-4s a warm request takes.
constexpr uint32_t HTTP_TIMEOUT_MS = 45000;
// Mirrors the bridge's MARGINALIA_TRANSLATE_MAX_CHARS. Enforced here too so an
// over-long selection fails instantly instead of spending WiFi and a round trip
// to be refused with 413.
constexpr size_t MAX_TEXT_CHARS = 600;
// A definition-sized reply plus JSON envelope. Anything larger is a misbehaving
// endpoint, not a translation.
constexpr size_t MAX_BODY_BYTES = 8192;

Result classify(const int code) {
  if (code == 200) return Result::Ok;
  if (code == 401 || code == 403) return Result::Unauthorized;
  if (code == 413) return Result::TooLong;
  if (code >= 500) return Result::ServerError;
  if (code <= 0) return Result::Network;
  return Result::BadResponse;
}

}  // namespace

Response translate(const std::string& text, const std::string& context, const std::string& title,
                   const std::string& author) {
  Response out;

  if (!TRANSLATE_STORE.isConfigured()) {
    out.result = Result::NotConfigured;
    return out;
  }
  if (text.empty()) {
    out.result = Result::BadResponse;
    return out;
  }
  if (text.size() > MAX_TEXT_CHARS) {
    out.result = Result::TooLong;
    return out;
  }
  if (WiFi.status() != WL_CONNECTED) {
    out.result = Result::NoWifi;
    return out;
  }

  JsonDocument req;
  req["text"] = text;
  req["target_lang"] = TRANSLATE_STORE.getTargetLang();
  if (!context.empty() && context != text) req["context"] = context;
  if (!title.empty()) req["book_title"] = title;
  if (!author.empty()) req["book_author"] = author;
  std::string payload;
  serializeJson(req, payload);

  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  if (!http.begin(TRANSLATE_STORE.getTranslateUrl())) {
    out.result = Result::Network;
    return out;
  }
  http.addHeader("Content-Type", "application/json");
  const std::string& token = TRANSLATE_STORE.getToken();
  if (!token.empty()) http.addHeader("X-Marginalia-Token", token);

  const int code = http.POST(payload);
  out.httpStatus = code;
  // The body carries the server's own error reason on failure, so read it before
  // deciding what to show — a 500 with "bedrock unavailable" is far more useful
  // on screen than a bare status code.
  std::string body = http.getString();
  http.end();

  if (body.size() > MAX_BODY_BYTES) {
    LOG_ERR("XLATE", "response too large (%u bytes)", static_cast<unsigned>(body.size()));
    out.result = Result::BadResponse;
    return out;
  }

  JsonDocument doc;
  const DeserializationError jerr = deserializeJson(doc, body);
  if (!jerr) {
    const char* err = doc["error"] | "";
    if (err[0] != '\0') out.error = err;
    const char* translation = doc["translation"] | "";
    if (translation[0] != '\0') out.translation = translation;
  }

  out.result = classify(code);
  // A 200 whose body had no usable translation is a failure, not an empty
  // definition: never paint a blank pane that looks like a successful lookup.
  if (out.result == Result::Ok && out.translation.empty()) {
    LOG_ERR("XLATE", "200 with no translation in body");
    out.result = Result::BadResponse;
  }
  if (out.result != Result::Ok) {
    LOG_ERR("XLATE", "translate -> %d (%s)", code, out.error.empty() ? "no reason given" : out.error.c_str());
  }
  return out;
}

const char* describe(const Result result) {
  switch (result) {
    case Result::Ok:
      return "";
    case Result::NotConfigured:
      return tr(STR_TRANSLATE_NOT_CONFIGURED);
    case Result::NoWifi:
      return tr(STR_TRANSLATE_NO_WIFI);
    case Result::Unauthorized:
      return tr(STR_TRANSLATE_UNAUTHORIZED);
    case Result::TooLong:
      return tr(STR_TRANSLATE_TOO_LONG);
    case Result::ServerError:
    case Result::BadResponse:
    case Result::Network:
    default:
      return tr(STR_TRANSLATE_FAILED);
  }
}

}  // namespace TranslateClient
