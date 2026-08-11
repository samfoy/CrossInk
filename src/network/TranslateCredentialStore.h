#pragma once

#include <PersistableStore.h>

#include <string>

/**
 * Singleton storing the marginalia bridge endpoint used by on-demand translation.
 *
 * The token is XOR-obfuscated with the device's hardware MAC and base64-encoded
 * before it hits the SD card, exactly as KOReaderCredentialStore treats the sync
 * password — not cryptographically secure, but it keeps the shared secret out of
 * plain sight on a card that gets mounted on other machines, and ties it to this
 * device.
 *
 * Stored separately from koreader.json because the two are independent services:
 * a reader may sync progress without translation configured, or the reverse, and
 * clearing one must not disturb the other.
 */
class TranslateCredentialStore : public PersistableStore<TranslateCredentialStore> {
  friend class PersistableStore<TranslateCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/translate.json"; }

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Base URL of the bridge, without a trailing slash (e.g.
  // "https://samfp.tech/marginalia"). Empty disables translation.
  void setBaseUrl(const std::string& url);
  const std::string& getBaseUrl() const { return baseUrl; }

  // Shared secret sent as the X-Marginalia-Token header. May be empty when the
  // bridge is unauthenticated (e.g. reached over a trusted tailnet).
  void setToken(const std::string& token);
  const std::string& getToken() const { return token; }

  // Language translations target, as a plain name the model understands
  // ("English", "Spanish"). Never empty — falls back to English.
  void setTargetLang(const std::string& lang);
  const std::string& getTargetLang() const { return targetLang; }

  // True when a translation request can actually be attempted. A URL is the only
  // hard requirement; the token is optional.
  bool isConfigured() const { return !baseUrl.empty(); }

  void clear();

  // Full endpoint for a translate request, or an empty string when unconfigured.
  std::string getTranslateUrl() const;

 private:
  TranslateCredentialStore() = default;

  std::string baseUrl;
  std::string token;
  std::string targetLang = "English";
};

#define TRANSLATE_STORE TranslateCredentialStore::getInstance()
