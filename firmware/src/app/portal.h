// Configuration portal: a Wi-Fi access point with a captive web page, started only
// on first boot or when the config button is used. Nothing listens otherwise.
#pragma once

#include <string>

#include "inkfetch/config.h"

namespace inkfetch::portal {

struct Status {
    std::string lastRefresh;  // human-readable, e.g. "12 min ago"
    std::string lastError;
};

// Runs until the settings are valid and Wi-Fi works, or the timeout expires.
// Returns true when the frame is ready for normal operation (the caller restarts).
bool run(Config& config, const Status& status, bool showOnPanel);

}  // namespace inkfetch::portal
