#include "inkfetch/ota.h"

#include <ArduinoJson.h>

#include <cctype>
#include <cstdlib>

#include "inkfetch/http.h"

namespace inkfetch {

namespace {

bool readNumber(const char*& p, unsigned& out) {
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    unsigned long value = 0;
    for (; std::isdigit(static_cast<unsigned char>(*p)); ++p) {
        value = value * 10 + unsigned(*p - '0');
        if (value > 999999) return false;
    }
    out = unsigned(value);
    return true;
}

bool isSha256(const std::string& hash) {
    if (hash.size() != 64) return false;
    for (const char c : hash) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && !(c >= 'a' && c <= 'f')) return false;
    }
    return true;
}

}  // namespace

std::optional<Version> parseVersion(const std::string& text) {
    const char* p = text.c_str();
    if (*p == 'v') ++p;
    Version v{};
    if (!readNumber(p, v.major) || *p++ != '.') return std::nullopt;
    if (!readNumber(p, v.minor) || *p++ != '.') return std::nullopt;
    if (!readNumber(p, v.patch) || *p != '\0') return std::nullopt;
    return v;
}

bool isNewer(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major > b.major;
    if (a.minor != b.minor) return a.minor > b.minor;
    return a.patch > b.patch;
}

const char* describe(ManifestError error) {
    switch (error) {
        case ManifestError::None: return "ok";
        case ManifestError::InvalidJson: return "manifest is not valid JSON";
        case ManifestError::MissingField: return "manifest lacks env, version, url or sha256";
        case ManifestError::BadVersion: return "manifest version is not x.y.z";
        case ManifestError::BadUrl: return "manifest url is not http(s)";
        case ManifestError::BadHash: return "manifest sha256 is not 64 lowercase hex chars";
    }
    return "unknown error";
}

ManifestError parseManifest(const char* json, size_t length, Manifest& out) {
    JsonDocument doc;
    if (deserializeJson(doc, json, length) != DeserializationError::Ok || !doc.is<JsonObject>()) {
        return ManifestError::InvalidJson;
    }
    const char* env = doc["env"];
    const char* version = doc["version"];
    const char* url = doc["url"];
    const char* sha256 = doc["sha256"];
    if (!env || !version || !url || !sha256) return ManifestError::MissingField;

    const auto parsed = parseVersion(version);
    if (!parsed) return ManifestError::BadVersion;
    if (!isHttpUrl(url)) return ManifestError::BadUrl;
    if (!isSha256(sha256)) return ManifestError::BadHash;

    out = Manifest{env, *parsed, url, sha256};
    return ManifestError::None;
}

bool shouldUpdate(const Manifest& manifest, const std::string& env, const Version& current) {
    return manifest.env == env && isNewer(manifest.version, current);
}

}  // namespace inkfetch
