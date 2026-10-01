#include "inkfetch/panel.h"

namespace inkfetch {

uint8_t nearestNative(Rgb c, const Rgb* palette, size_t count) {
    uint8_t best = 0;
    uint32_t bestDistance = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) {
        const int dr = int(c.r) - palette[i].r;
        const int dg = int(c.g) - palette[i].g;
        const int db = int(c.b) - palette[i].b;
        const uint32_t distance = uint32_t(dr * dr + dg * dg + db * db);
        if (distance < bestDistance) {
            best = uint8_t(i);
            bestDistance = distance;
        }
    }
    return best;
}

}  // namespace inkfetch
