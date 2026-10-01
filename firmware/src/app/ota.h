// Pull-based firmware updates with automatic rollback.
//
// A new image boots in the bootloader's "pending verify" state. It is confirmed only
// once a cycle has reached the image server; if it cannot, or if it crashes, the
// bootloader goes back to the previous image on the next boot.
#pragma once

#include <string>

namespace inkfetch::ota {

enum class Outcome { UpToDate, Updated, Failed };

// Reads the manifest and, if it targets this build with a newer version, downloads,
// verifies (SHA-256) and installs the binary. The caller restarts on Updated.
Outcome run(const std::string& manifestUrl);

bool pendingVerify();
void confirm();
[[noreturn]] void rollback();

}  // namespace inkfetch::ota
