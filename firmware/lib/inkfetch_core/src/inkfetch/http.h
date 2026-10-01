// Small HTTP helpers: request URL and response headers.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace inkfetch {

using QueryParams = std::vector<std::pair<std::string, std::string>>;

// Appends `params` to `base` (which may already have a query string), percent-
// encoding values. Empty values are skipped.
std::string withQuery(const std::string& base, const QueryParams& params);

// `Retry-After: <seconds>`. The HTTP-date form is not supported (the frame does not
// know the time) and, like anything invalid, yields no value.
std::optional<uint32_t> parseRetryAfter(const char* value);

bool isHttpUrl(const std::string& url);
bool isHttpsUrl(const std::string& url);

}  // namespace inkfetch
