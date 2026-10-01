#include "inkfetch/png_frame.h"

#include <PNGdec.h>

#include <memory>
#include <new>
#include <vector>

namespace inkfetch {

namespace {

struct DecodeState {
    const PanelInfo* panel;
    RowSink sink;
    void* context;
    std::vector<uint8_t> row;
    uint8_t map[256];
    bool mapped = false;
    int rows = 0;  // a truncated stream can end "successfully" early
};

// PNGdec keeps its whole state (~50 KB) in the object: never on the stack.
std::unique_ptr<PNG> makeDecoder() {
    return std::unique_ptr<PNG>(new (std::nothrow) PNG());
}

int onRow(PNGDRAW* draw) {
    auto* state = static_cast<DecodeState*>(draw->pUser);
    if (!state->mapped) {
        // The palette is parsed before the first row; unused entries are zero.
        const uint8_t* plte = draw->pPalette;
        for (int i = 0; i < 256; ++i) {
            const Rgb c{plte[i * 3], plte[i * 3 + 1], plte[i * 3 + 2]};
            state->map[i] = nearestNative(c, state->panel->palette, state->panel->paletteSize);
        }
        state->mapped = true;
    }

    // Rows are packed MSB first at 1, 2, 4 or 8 bits per pixel.
    const int bpp = draw->iBpp;
    const int perByte = 8 / bpp;
    const uint8_t mask = uint8_t((1 << bpp) - 1);
    for (int x = 0; x < draw->iWidth; ++x) {
        const uint8_t byte = draw->pPixels[x / perByte];
        const int shift = 8 - bpp * (x % perByte + 1);
        state->row[x] = state->map[(byte >> shift) & mask];
    }
    state->sink(state->context, uint16_t(draw->y), state->row.data(), uint16_t(draw->iWidth));
    ++state->rows;
    return 1;  // keep going
}

FrameError open(PNG& png, const uint8_t* data, size_t length, const PanelInfo& panel,
                PNG_DRAW_CALLBACK* draw) {
    if (length < 8 || png.openRAM(const_cast<uint8_t*>(data), int(length), draw) != PNG_SUCCESS) {
        return FrameError::NotPng;
    }
    if (png.getWidth() != panel.width || png.getHeight() != panel.height) {
        return FrameError::WrongSize;
    }
    if (png.getPixelType() != PNG_PIXEL_INDEXED) {
        return FrameError::NotIndexed;
    }
    return FrameError::None;
}

}  // namespace

const char* describe(FrameError error) {
    switch (error) {
        case FrameError::None: return "ok";
        case FrameError::NotPng: return "not a supported PNG";
        case FrameError::WrongSize: return "PNG size differs from the panel";
        case FrameError::NotIndexed: return "PNG is not indexed (palette)";
        case FrameError::Corrupt: return "PNG data is corrupt or truncated";
        case FrameError::NoMemory: return "out of memory";
    }
    return "unknown error";
}

FrameError checkFrame(const uint8_t* data, size_t length, const PanelInfo& panel) {
    auto png = makeDecoder();
    if (!png) return FrameError::NoMemory;
    const FrameError error = open(*png, data, length, panel, nullptr);
    png->close();
    return error;
}

FrameError decodeFrame(const uint8_t* data, size_t length, const PanelInfo& panel,
                       RowSink sink, void* context) {
    auto png = makeDecoder();
    if (!png) return FrameError::NoMemory;
    FrameError error = open(*png, data, length, panel, onRow);
    if (error != FrameError::None) {
        png->close();
        return error;
    }

    DecodeState state{&panel, sink, context, std::vector<uint8_t>(panel.width), {}};
    const int result = png->decode(&state, 0);
    png->close();
    if (result != PNG_SUCCESS || state.rows != panel.height) {
        return FrameError::Corrupt;
    }
    return FrameError::None;
}

}  // namespace inkfetch
