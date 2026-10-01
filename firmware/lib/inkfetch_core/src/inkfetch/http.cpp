#include "inkfetch/http.h"

#include <cctype>
#include <cstring>

namespace inkfetch {

namespace {

std::string encode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

}  // namespace

std::string withQuery(const std::string& base, const QueryParams& params) {
    // Keep a fragment, if any, at the very end.
    const size_t hash = base.find('#');
    std::string url = base.substr(0, hash);
    const std::string fragment = hash == std::string::npos ? "" : base.substr(hash);

    char separator = url.find('?') == std::string::npos ? '?' : '&';
    if (!url.empty() && (url.back() == '?' || url.back() == '&')) separator = '\0';
    for (const auto& [key, value] : params) {
        if (value.empty()) continue;
        if (separator) url += separator;
        url += encode(key) + "=" + encode(value);
        separator = '&';
    }
    return url + fragment;
}

std::optional<uint32_t> parseRetryAfter(const char* value) {
    if (!value) return std::nullopt;
    while (*value == ' ' || *value == '\t') ++value;
    if (!std::isdigit(static_cast<unsigned char>(*value))) return std::nullopt;
    uint64_t seconds = 0;
    for (; std::isdigit(static_cast<unsigned char>(*value)); ++value) {
        seconds = seconds * 10 + uint64_t(*value - '0');
        if (seconds > UINT32_MAX) return std::nullopt;
    }
    while (*value == ' ' || *value == '\t') ++value;
    if (*value != '\0') return std::nullopt;
    return uint32_t(seconds);
}

bool isHttpUrl(const std::string& url) {
    return (startsWith(url, "http://") && url.size() > 7) || isHttpsUrl(url);
}

bool isHttpsUrl(const std::string& url) {
    return startsWith(url, "https://") && url.size() > 8;
}

}  // namespace inkfetch
