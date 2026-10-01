// Decoding of the frame PNG sent by the server.
//
// The server does all the image work: the PNG is indexed, already dithered and at
// the panel's size. Decoding only maps each PNG palette entry to the nearest native
// ink and hands rows of native indices to the display.
#pragma once

#include <cstddef>
#include <cstdint>

#include "inkfetch/panel.h"

namespace inkfetch {

enum class FrameError {
    None,
    NotPng,      // not a PNG, or a PNG variant the decoder refuses (e.g. interlaced)
    WrongSize,   // dimensions differ from the panel
    NotIndexed,  // true colour or greyscale: the server must dither for the panel
    Corrupt,     // truncated or damaged data
    NoMemory,
};

const char* describe(FrameError error);

// Receives one decoded row: `width` native palette indices.
using RowSink = void (*)(void* context, uint16_t y, const uint8_t* row, uint16_t width);

// Checks the PNG header against the panel without decoding the pixels, so the
// display is only powered up for a frame that can be shown.
FrameError checkFrame(const uint8_t* png, size_t length, const PanelInfo& panel);

// Decodes the whole frame, calling `sink` once per row, top to bottom.
FrameError decodeFrame(const uint8_t* png, size_t length, const PanelInfo& panel,
                       RowSink sink, void* context);

}  // namespace inkfetch
