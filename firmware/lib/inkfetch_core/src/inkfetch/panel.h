// What the rest of the firmware needs to know about a panel. Each display profile
// fills one of these; nothing in core/ depends on a particular screen.
#pragma once

#include <cstddef>
#include <cstdint>

namespace inkfetch {

struct Rgb {
    uint8_t r, g, b;
};

// Protection limits for the glass, from the panel vendor's guidance.
struct Limits {
    uint32_t minRefreshS;  // at least this long between two refreshes
    uint32_t maxStaleS;    // refresh at least this often, even for the same image
};

struct PanelInfo {
    uint16_t width;
    uint16_t height;
    const char* paletteName;  // announced to the server, e.g. "spectra6"
    const Rgb* palette;       // native inks; a frame row holds indices into this
    size_t paletteSize;
    Limits limits;
};

// Index of the native colour nearest to `c`: squared RGB distance, lowest index on
// ties. Must match `nearest_native` in tools/src/inkfetch/palettes.py.
uint8_t nearestNative(Rgb c, const Rgb* palette, size_t count);

}  // namespace inkfetch
