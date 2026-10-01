// Configuration persisted in flash (NVS), plus the few values that must survive a
// power loss (current mode, portal password).
#pragma once

#include <string>

#include "inkfetch/config.h"

namespace inkfetch::settings {

// Loads the stored configuration, starting from defaults (and the panel limits)
// for anything missing. `secrets.h`, when present, provides development defaults.
Config load(const Limits& panel);
void save(const Config& config);

// Index into splitModes(config.modes).
size_t modeIndex();
void setModeIndex(size_t index);

// Password of the configuration access point: random, generated once.
std::string portalPassword();

}  // namespace inkfetch::settings
