#include "TranslateCredentialStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

namespace {
// Keeps a stray newline or trailing slash from a copy-paste out of the request
// URL, where "…/marginalia/" + "/translate" would 404.
std::string sanitizeUrl(const std::string& raw) {
  size_t begin = raw.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  size_t end = raw.find_last_not_of(" \t\r\n/");
  if (end == std::string::npos || end < begin) return {};
  return raw.substr(begin, end - begin + 1);
}

std::string sanitizeToken(const std::string& raw) {
  const size_t begin = raw.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const size_t end = raw.find_last_not_of(" \t\r\n");
  return raw.substr(begin, end - begin + 1);
}
}  // namespace

void TranslateCredentialStore::toJson(JsonDocument& doc) const {
  doc["baseUrl"] = baseUrl;
  // Reuses the "password_obf" key so PersistableStoreBase::extractPassword can
  // read it back, including its plaintext-fallback migration path.
  doc["password_obf"] = obfuscation::obfuscateToBase64(token);
  doc["targetLang"] = targetLang;
}

bool TranslateCredentialStore::fromJson(JsonVariantConst doc) {
  setBaseUrl(doc["baseUrl"] | "");

  bool needsResave = false;
  setToken(extractPassword(doc, needsResave));
  if (needsResave) requestResave();

  setTargetLang(doc["targetLang"] | "");
  return true;
}

void TranslateCredentialStore::setBaseUrl(const std::string& url) { baseUrl = sanitizeUrl(url); }

void TranslateCredentialStore::setToken(const std::string& newToken) { token = sanitizeToken(newToken); }

void TranslateCredentialStore::setTargetLang(const std::string& lang) {
  const std::string trimmed = sanitizeToken(lang);
  // An empty target would make the model guess; English is the useful default
  // for a reader hitting a foreign phrase in an English-language book.
  targetLang = trimmed.empty() ? "English" : trimmed;
}

void TranslateCredentialStore::clear() {
  baseUrl.clear();
  token.clear();
  targetLang = "English";
  saveToFile();
}

std::string TranslateCredentialStore::getTranslateUrl() const {
  if (baseUrl.empty()) return {};
  return baseUrl + "/translate";
}
