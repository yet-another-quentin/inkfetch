#include "app/settings.h"

#include <Preferences.h>
#include <esp_random.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace inkfetch::settings {

namespace {

constexpr const char* kNamespace = "inkfetch";

std::string getString(Preferences& p, const char* key, const std::string& fallback) {
    return p.isKey(key) ? std::string(p.getString(key).c_str()) : fallback;
}

}  // namespace

Config load(const Limits& panel) {
    Config c;
    c.limits = panel;
#ifdef INKFETCH_DEFAULT_IMAGE_URL
    c.imageUrl = INKFETCH_DEFAULT_IMAGE_URL;
#endif
#ifdef INKFETCH_DEFAULT_TOKEN
    c.token = INKFETCH_DEFAULT_TOKEN;
#endif
#ifdef INKFETCH_DEFAULT_OTA_MANIFEST_URL
    c.otaManifestUrl = INKFETCH_DEFAULT_OTA_MANIFEST_URL;
#endif

    Preferences p;
    if (!p.begin(kNamespace, true)) return c;  // nothing stored yet
    c.hostname = getString(p, "hostname", c.hostname);
    c.imageUrl = getString(p, "imageUrl", c.imageUrl);
    c.token = getString(p, "token", c.token);
    c.modes = getString(p, "modes", c.modes);
    c.pollS = p.getUInt("pollS", c.pollS);
    c.honorRetryAfter = p.getBool("retryAfter", c.honorRetryAfter);
    c.otaManifestUrl = getString(p, "otaUrl", c.otaManifestUrl);
    c.otaEveryHours = p.getUInt("otaHours", c.otaEveryHours);
    // Stored limits can only be more cautious than the panel's (validated on save),
    // but a firmware update may have tightened the panel's own: take the stricter.
    c.limits.minRefreshS = std::max(panel.minRefreshS, uint32_t(p.getUInt("minRefresh", panel.minRefreshS)));
    c.limits.maxStaleS = std::min(panel.maxStaleS, uint32_t(p.getUInt("maxStale", panel.maxStaleS)));
    p.end();
    return c;
}

void save(const Config& c) {
    Preferences p;
    p.begin(kNamespace, false);
    p.putString("hostname", c.hostname.c_str());
    p.putString("imageUrl", c.imageUrl.c_str());
    p.putString("token", c.token.c_str());
    p.putString("modes", c.modes.c_str());
    p.putUInt("pollS", c.pollS);
    p.putBool("retryAfter", c.honorRetryAfter);
    p.putString("otaUrl", c.otaManifestUrl.c_str());
    p.putUInt("otaHours", c.otaEveryHours);
    p.putUInt("minRefresh", c.limits.minRefreshS);
    p.putUInt("maxStale", c.limits.maxStaleS);
    p.end();
}

size_t modeIndex() {
    Preferences p;
    if (!p.begin(kNamespace, true)) return 0;
    const size_t index = p.getUInt("modeIndex", 0);
    p.end();
    return index;
}

void setModeIndex(size_t index) {
    Preferences p;
    p.begin(kNamespace, false);
    p.putUInt("modeIndex", uint32_t(index));
    p.end();
}

std::string portalPassword() {
    Preferences p;
    p.begin(kNamespace, false);
    std::string password = getString(p, "apPassword", "");
    if (password.size() < 8) {
        // No look-alike characters: it is read off the e-paper screen.
        static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
        password.clear();
        for (int i = 0; i < 10; ++i) password += alphabet[esp_random() % (sizeof alphabet - 1)];
        p.putString("apPassword", password.c_str());
    }
    p.end();
    return password;
}

}  // namespace inkfetch::settings
