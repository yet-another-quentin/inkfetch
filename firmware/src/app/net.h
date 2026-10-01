// Wi-Fi and HTTP(S) downloads.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace inkfetch::net {

// Connects with the credentials saved by the configuration portal.
bool connect(const std::string& hostname, uint32_t timeoutMs);
// Whether Wi-Fi credentials are stored (storing the development ones from
// secrets.h first, if defined and nothing is stored yet).
bool hasCredentials();
void disconnect();
std::string macId();  // e.g. "a1b2c3d4e5f6", sent to servers as `id`

struct Buffer {
    uint8_t* data = nullptr;  // PSRAM when available
    size_t length = 0;
    size_t capacity = 0;
    ~Buffer();
};

struct Response {
    int status = 0;  // HTTP status, or a negative HTTPClient error
    bool reachedServer = false;  // an HTTP response arrived, even if the body then failed
    std::string error;
    std::string etag;
    std::optional<uint32_t> retryAfter;
    std::unique_ptr<Buffer> body;  // only for 200
};

struct Request {
    std::string url;
    std::string bearerToken;  // empty: no Authorization header
    std::string ifNoneMatch;  // empty: unconditional
    size_t maxBytes;
};

// GET; https:// uses the built-in certificate bundle. Redirects are followed, except
// with a bearer token (HTTPClient would forward it to the redirect target).
Response get(const Request& request);

}  // namespace inkfetch::net
