// Profile: Seeed Studio XIAO ePaper Display Board EE02 with the 13.3" E Ink
// Spectra 6 panel (T133A01, 1200x1600 native, driven in landscape 1600x1200).
//
// Pins (from Seeed_GFX2's board definition): SCK 7, MOSI 9, CS 44 + 41 (dual
// controller), DC 10, BUSY 4, RST 38, panel power 43. User buttons 2 / 3 / 5.
#include <Arduino.h>
#include <driver/gpio.h>
#include <qrcode.h>

#include <Seeed_GFX.h>
#include "board/boards/XIAO_ePaper_Boards.h"
#include "driver/epaper/Driver_T133A01.h"
#include "panel/Panel_EPaper.h"
#include "panel/configs/Seeed_Panel_Configs.h"

#include "profile.h"

namespace inkfetch {

namespace {

constexpr uint16_t kWidth = 1600;
constexpr uint16_t kHeight = 1200;
constexpr gpio_num_t kPanelPower = GPIO_NUM_43;

// Native order: black, white, yellow, red, blue, green (tools/src/inkfetch/palettes.py).
const Rgb kPalette[] = {{0, 0, 0}, {255, 255, 255}, {255, 255, 0}, {255, 0, 0}, {0, 0, 255}, {0, 255, 0}};
// The same inks in Seeed_GFX2's 4 bpp framebuffer codes.
const uint8_t kSeeedCode[] = {0xF, 0x0, 0xB, 0x6, 0xD, 0x2};
const uint16_t kTftColour[] = {TFT_BLACK, TFT_WHITE, TFT_YELLOW, TFT_RED, TFT_BLUE, TFT_GREEN};

// Waveshare / E Ink guidance for Spectra 6 panels: at least 180 s between two
// refreshes, and at least one refresh every 24 h.
const PanelInfo kInfo{kWidth, kHeight, "spectra6", kPalette, 6, {180, 24 * 3600}};

const Buttons kButtons{2, 3, 5};

Seeed_GFX gfx;

// Drawing target of the QR code callback (esp_qrcode has no user pointer).
struct QrTarget {
    int32_t x, y, maxSide;
} qrTarget;

void drawQr(esp_qrcode_handle_t qr) {
    const int size = esp_qrcode_get_size(qr);
    const int quiet = 2;  // modules of white border
    const int32_t module = qrTarget.maxSide / (size + 2 * quiet);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (esp_qrcode_get_module(qr, x, y)) {
                gfx.fillRect(qrTarget.x + (x + quiet) * module, qrTarget.y + (y + quiet) * module, module, module,
                             TFT_BLACK);
            }
        }
    }
}

class SeeedEE02 : public Display {
public:
    const PanelInfo& info() const override { return kInfo; }

    bool begin() override {
        if (_begun) return true;
        gpio_hold_dis(kPanelPower);  // held low during the last deep sleep
        if (!gfx.begin<Board_XIAO_ePaper_EE02, Config_Seeed_ePaper_13inch3_Colorful_T133A01>()) {
            log_e("display begin failed: %s", gfx.lastResult().message);
            return false;
        }
        gfx.setRotation(1);  // native 1200x1600 portrait -> 1600x1200 landscape
        _begun = true;
        return true;
    }

    void writeRow(uint16_t y, const uint8_t* row) override {
        for (uint16_t x = 0; x < kWidth; x += 2) {
            _packed[x / 2] = uint8_t(kSeeedCode[row[x]] << 4 | kSeeedCode[row[x + 1]]);
        }
        gfx.pushImage4BPP(0, y, kWidth, 1, _packed);
    }

    void fill(uint8_t nativeIndex) override { gfx.fillScreen(kTftColour[nativeIndex]); }

    void drawMessage(const Message& m) override {
        gfx.fillScreen(TFT_WHITE);
        gfx.setTextColor(TFT_BLACK);
        gfx.setTextSize(7);
        gfx.drawString(m.title.c_str(), 80, 80);
        gfx.setTextSize(4);
        int32_t y = 240;
        for (const auto& line : m.lines) {
            gfx.drawString(line.c_str(), 80, y);
            y += 70;
        }
        if (!m.qr.empty()) {
            qrTarget = {kWidth - 80 - 520, kHeight - 80 - 520, 520};
            esp_qrcode_config_t config = ESP_QRCODE_CONFIG_DEFAULT();
            config.display_func = drawQr;
            if (esp_qrcode_generate(&config, m.qr.c_str()) != ESP_OK) log_e("QR code generation failed");
        }
    }

    bool refresh() override {
        const GfxResult result = gfx.refresh();
        if (!result) log_e("refresh failed: %s", result.message);
        return bool(result);
    }

    void powerOff() override {
        if (_begun) {
            gfx.end();  // panel deep sleep, then power pin low
            _begun = false;
        } else {
            pinMode(kPanelPower, OUTPUT);
            digitalWrite(kPanelPower, LOW);
        }
        // Keep the panel unpowered while the MCU sleeps.
        gpio_hold_en(kPanelPower);
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
        gpio_deep_sleep_hold_en();
#endif
    }

private:
    bool _begun = false;
    uint8_t _packed[kWidth / 2];
};

SeeedEE02 instance;

}  // namespace

Display& display() { return instance; }
const Buttons& buttons() { return kButtons; }

}  // namespace inkfetch
