#pragma once

#include <string>

/**
 * Client for the marginalia bridge's POST /translate endpoint.
 *
 * The bridge answers with a translation produced live by a strong model, which
 * is why this exists rather than an on-device translation table: the reader
 * selects arbitrary text on the page and there is no precomputed index for it.
 *
 * Blocking: a call takes roughly 1.5-4s against the real bridge, so callers must
 * paint a "translating…" state before invoking it and must already hold WiFi.
 */
namespace TranslateClient {

enum class Result : uint8_t {
  Ok,
  NotConfigured,  // no bridge URL stored
  NoWifi,         // WiFi is not connected
  Network,        // could not connect / TLS failure / timeout
  Unauthorized,   // 401/403 — token missing or wrong
  TooLong,        // 413 — selection exceeded the bridge's cap
  ServerError,    // 5xx, including an empty completion (502)
  BadResponse,    // 2xx whose body was not the expected JSON shape
};

struct Response {
  Result result = Result::NotConfigured;
  std::string translation;  // populated only when result == Ok
  std::string error;        // server-supplied reason, when it gave one
  int httpStatus = 0;
};

/**
 * Translate `text` into the configured target language.
 *
 * @param text     the reader's selection; must be non-empty
 * @param context  surrounding sentence for disambiguating a single word, sent
 *                 explicitly marked do-not-translate. May be empty.
 * @param title    book title, for register/context. May be empty.
 * @param author   book author. May be empty.
 */
Response translate(const std::string& text, const std::string& context = {}, const std::string& title = {},
                   const std::string& author = {});

// Human-readable, translated message for a failed result, for painting on screen.
const char* describe(Result result);

}  // namespace TranslateClient
