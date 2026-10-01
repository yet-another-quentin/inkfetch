// Frame settings, as entered in the configuration portal and stored in flash.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "inkfetch/panel.h"

namespace inkfetch {

struct Config {
    std::string hostname = "inkfetch";
    std::string imageUrl;
    std::string token;           // optional bearer token for imageUrl
    std::string modes;           // comma-separated, cycled by the mode button; may be empty
    uint32_t pollS = 900;
    bool honorRetryAfter = true;
    std::string otaManifestUrl;  // empty: OTA disabled
    uint32_t otaEveryHours = 24;
    Limits limits{};             // never less cautious than the panel's own limits

    bool complete() const { return !imageUrl.empty(); }
};

// Raw form values. Numbers are text so that bad input can be reported.
struct ConfigForm {
    std::string hostname, imageUrl, token, modes, pollS, otaManifestUrl, otaEveryHours,
        minRefreshS, maxStaleS;
    bool honorRetryAfter = true;
    bool clearToken = false;  // an empty token field keeps the stored one
};

// Checks `form` against the panel's limits. On success fills `out` from `form`
// (starting from `previous`, which keeps the stored token when the field is empty)
// and returns no errors; otherwise `out` is untouched.
std::vector<std::string> validate(const ConfigForm& form, const Limits& panel, const Config& previous,
                                  Config& out);

// The form pre-filled from `config` (the token is never echoed back).
ConfigForm toForm(const Config& config);

std::vector<std::string> splitModes(const std::string& modes);

// Payload of a "join this Wi-Fi" QR code: WIFI:T:WPA;S:<ssid>;P:<password>;;
std::string wifiQrPayload(const std::string& ssid, const std::string& password);

}  // namespace inkfetch
