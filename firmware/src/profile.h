// Everything that depends on the hardware, behind one interface. Each display
// profile (src/display/<profile>.cpp, selected by the PlatformIO environment)
// implements `display()` and `buttons()`; the rest of the firmware never talks to
// a screen or a board directly. See "Adding a display" in the README.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "inkfetch/panel.h"

namespace inkfetch {

// A text page with an optional QR code, drawn by the profile with its own library.
struct Message {
    std::string title;
    std::vector<std::string> lines;
    std::string qr;  // empty: no QR code
};

class Display {
public:
    virtual ~Display() = default;

    virtual const PanelInfo& info() const = 0;

    // Powers the panel up and prepares the framebuffer.
    virtual bool begin() = 0;
    // One row of `info().width` native palette indices (see PanelInfo::palette).
    virtual void writeRow(uint16_t y, const uint8_t* row) = 0;
    virtual void fill(uint8_t nativeIndex) = 0;
    virtual void drawMessage(const Message& message) = 0;
    // Sends the framebuffer to the glass. Slow (tens of seconds on colour panels).
    virtual bool refresh() = 0;
    // Panel deep sleep and power cut, kept through the MCU's deep sleep. Safe to call
    // when begin() was never called.
    virtual void powerOff() = 0;
};

// Active-low buttons on RTC-capable GPIOs (they wake the board); -1 when absent.
struct Buttons {
    int8_t refresh;  // short press: refresh now; long press: blank for storage
    int8_t mode;     // next mode in the configured list
    int8_t config;   // held at boot or pressed while asleep: configuration portal
};

Display& display();
const Buttons& buttons();

}  // namespace inkfetch
