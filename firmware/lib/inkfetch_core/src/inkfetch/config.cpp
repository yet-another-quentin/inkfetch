#include "inkfetch/config.h"

#include <cctype>
#include <optional>

#include "inkfetch/http.h"

namespace inkfetch {

namespace {

std::string trim(const std::string& s) {
    const size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
}

std::optional<uint32_t> parseUint(const std::string& text, uint32_t lo, uint32_t hi) {
    const std::string s = trim(text);
    if (s.empty() || s.size() > 10) return std::nullopt;
    uint64_t value = 0;
    for (const char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        value = value * 10 + uint64_t(c - '0');
    }
    if (value < lo || value > hi) return std::nullopt;
    return uint32_t(value);
}

bool isHostname(const std::string& s) {
    if (s.empty() || s.size() > 32 || s.front() == '-' || s.back() == '-') return false;
    for (const char c : s) {
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

bool isModeList(const std::string& s) {
    for (const char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ',')) return false;
    }
    return true;
}

std::string range(uint32_t lo, uint32_t hi) {
    return " (" + std::to_string(lo) + " to " + std::to_string(hi) + ")";
}

}  // namespace

std::vector<std::string> validate(const ConfigForm& form, const Limits& panel, const Config& previous,
                                  Config& out) {
    std::vector<std::string> errors;
    Config next = previous;

    next.hostname = trim(form.hostname);
    if (!isHostname(next.hostname)) {
        errors.push_back("Hostname: 1 to 32 lowercase letters, digits or dashes");
    }

    next.imageUrl = trim(form.imageUrl);
    if (!isHttpUrl(next.imageUrl)) errors.push_back("Image URL: must start with http:// or https://");

    const std::string token = trim(form.token);
    if (form.clearToken) {
        next.token.clear();
    } else if (!token.empty()) {
        next.token = token;
    }

    next.modes = trim(form.modes);
    if (!isModeList(next.modes)) errors.push_back("Modes: comma-separated names (letters, digits, - and _)");

    if (const auto poll = parseUint(form.pollS, 60, 86400)) {
        next.pollS = *poll;
    } else {
        errors.push_back("Poll interval: whole seconds" + range(60, 86400));
    }
    next.honorRetryAfter = form.honorRetryAfter;

    next.otaManifestUrl = trim(form.otaManifestUrl);
    if (!next.otaManifestUrl.empty() && !isHttpUrl(next.otaManifestUrl)) {
        errors.push_back("OTA manifest URL: empty, or starting with http:// or https://");
    }
    if (const auto every = parseUint(form.otaEveryHours, 1, 720)) {
        next.otaEveryHours = *every;
    } else {
        errors.push_back("OTA check: whole hours" + range(1, 720));
    }

    // Protection settings may only be more cautious than the panel's own limits.
    const auto minRefresh = parseUint(form.minRefreshS, panel.minRefreshS, panel.maxStaleS);
    const auto maxStale = parseUint(form.maxStaleS, panel.minRefreshS, panel.maxStaleS);
    if (!minRefresh) errors.push_back("Minimum time between refreshes: seconds" + range(panel.minRefreshS, panel.maxStaleS));
    if (!maxStale) errors.push_back("Maximum time without refresh: seconds" + range(panel.minRefreshS, panel.maxStaleS));
    if (minRefresh && maxStale) {
        if (*minRefresh > *maxStale) {
            errors.push_back("Minimum time between refreshes must not exceed the maximum time without refresh");
        } else {
            next.limits = {*minRefresh, *maxStale};
        }
    }

    if (errors.empty()) out = next;
    return errors;
}

ConfigForm toForm(const Config& c) {
    ConfigForm form;
    form.hostname = c.hostname;
    form.imageUrl = c.imageUrl;
    form.modes = c.modes;
    form.pollS = std::to_string(c.pollS);
    form.honorRetryAfter = c.honorRetryAfter;
    form.otaManifestUrl = c.otaManifestUrl;
    form.otaEveryHours = std::to_string(c.otaEveryHours);
    form.minRefreshS = std::to_string(c.limits.minRefreshS);
    form.maxStaleS = std::to_string(c.limits.maxStaleS);
    return form;
}

std::vector<std::string> splitModes(const std::string& modes) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= modes.size()) {
        const size_t comma = modes.find(',', start);
        const std::string mode = trim(modes.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!mode.empty()) out.push_back(mode);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

std::string wifiQrPayload(const std::string& ssid, const std::string& password) {
    auto escape = [](const std::string& s) {
        std::string out;
        for (const char c : s) {
            if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out += '\\';
            out += c;
        }
        return out;
    };
    return "WIFI:T:WPA;S:" + escape(ssid) + ";P:" + escape(password) + ";;";
}

}  // namespace inkfetch
