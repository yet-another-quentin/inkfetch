// Optional development defaults. Copy to `secrets.h` (git-ignored) to skip the
// configuration portal while developing. Values saved in the portal always win.
#pragma once

// Pre-fills the image URL, so the frame starts fetching as soon as Wi-Fi is known.
#define INKFETCH_DEFAULT_IMAGE_URL "http://192.168.1.10:8080/frame.png"

// Stores these Wi-Fi credentials on first boot if none are saved yet.
#define INKFETCH_DEV_WIFI_SSID     "my-network"
#define INKFETCH_DEV_WIFI_PASSWORD "my-password"

// #define INKFETCH_DEFAULT_TOKEN "change-me"
// #define INKFETCH_DEFAULT_OTA_MANIFEST_URL "http://192.168.1.10:8080/firmware/seeed-ee02.json"
