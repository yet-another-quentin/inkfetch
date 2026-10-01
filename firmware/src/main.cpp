// inkfetch: an e-paper frame that pulls its picture.
//
// Each wake-up (timer or button): Wi-Fi, GET the image URL, redraw the panel only if
// the picture changed (or the panel needs its maintenance refresh), then deep sleep.
// The server does all the image work; the frame only decodes an indexed PNG. See
// the README for the contract between frame and server.
#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_rom_crc.h>
#include <esp_sleep.h>
#include <sys/time.h>

#include "app/net.h"
#include "app/ota.h"
#include "app/portal.h"
#include "app/settings.h"
#include "inkfetch/http.h"
#include "inkfetch/png_frame.h"
#include "inkfetch/policy.h"
#include "profile.h"

using namespace inkfetch;

namespace {

constexpr uint32_t kWifiTimeoutMs = 20000;
constexpr size_t kMaxPngBytes = 2 * 1024 * 1024;
constexpr uint32_t kLongPressMs = 3000;

// Kept in RTC memory. RTC_NOINIT (not RTC_DATA) so it also survives software,
// panic and brownout resets: the refresh limits must hold across them. Only a
// power-on (or corrupt memory) starts from scratch.
constexpr uint32_t kRtcMagic = 0x696e6b31;  // "ink1"
struct RtcState {
    uint32_t magic;
    RefreshState refresh;
    char etag[96];
    uint32_t crc;  // of the PNG last shown, for servers without ETag
    bool otaChecked;
    int64_t otaCheckedAt;
    bool pendingRefresh;  // a button press that came too soon after a refresh
    bool storage;         // blank the panel, then sleep until a button press
    char lastError[96];
    uint8_t crashes;  // consecutive panic / watchdog resets
};
RTC_NOINIT_ATTR RtcState rtc;

void loadRtcState() {
    if (esp_reset_reason() == ESP_RST_POWERON || rtc.magic != kRtcMagic) {
        rtc = RtcState{};
        rtc.magic = kRtcMagic;
    }
    rtc.etag[sizeof rtc.etag - 1] = '\0';
    rtc.lastError[sizeof rtc.lastError - 1] = '\0';
}

int64_t now() {
    // The RTC keeps counting through deep sleep; only differences matter.
    timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec;
}

void setError(const std::string& error) {
    strlcpy(rtc.lastError, error.c_str(), sizeof rtc.lastError);
    if (!error.empty()) log_w("%s", error.c_str());
}

std::string ago(int64_t seconds) {
    if (seconds < 120) return std::to_string(seconds) + " s ago";
    if (seconds < 7200) return std::to_string(seconds / 60) + " min ago";
    return std::to_string(seconds / 3600) + " h ago";
}

// --- sleep and wake -------------------------------------------------------------

uint64_t buttonMask() {
    uint64_t mask = 0;
    for (const int8_t pin : {buttons().refresh, buttons().mode, buttons().config}) {
        if (pin >= 0) mask |= 1ULL << pin;
    }
    return mask;
}

// Deep sleep for `seconds`, or until a button press only when `seconds` is 0.
[[noreturn]] void sleepFor(uint32_t seconds) {
    rtc.crashes = 0;  // reaching sleep means the cycle completed
    net::disconnect();
    display().powerOff();
    if (const uint64_t mask = buttonMask()) {
        for (int pin = 0; pin < 64; ++pin) {
            if (!(mask & (1ULL << pin))) continue;
            rtc_gpio_pullup_en(gpio_num_t(pin));
            rtc_gpio_pulldown_dis(gpio_num_t(pin));
        }
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);  // keeps the pull-ups
        esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    }
    if (seconds) esp_sleep_enable_timer_wakeup(uint64_t(seconds) * 1000000ULL);
    log_i("deep sleep %s", seconds ? (std::to_string(seconds) + " s").c_str() : "until a button press");
    Serial.flush();
    esp_deep_sleep_start();
}

struct Wake {
    Trigger trigger = Trigger::PowerOn;
    bool config = false;     // open the configuration portal
    bool longPress = false;  // refresh button held: blank for storage
};

bool held(int8_t pin) {
    if (pin < 0) return false;
    pinMode(pin, INPUT_PULLUP);
    delay(2);
    return digitalRead(pin) == LOW;
}

Wake readWake() {
    Wake wake;
    const Buttons& b = buttons();
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_TIMER:
            wake.trigger = Trigger::Timer;
            break;
        case ESP_SLEEP_WAKEUP_EXT1: {
            const uint64_t pins = esp_sleep_get_ext1_wakeup_status();
            if (b.config >= 0 && (pins & (1ULL << b.config))) {
                wake.config = true;
            } else if (b.mode >= 0 && (pins & (1ULL << b.mode))) {
                wake.trigger = Trigger::ModeButton;
            } else {
                wake.trigger = Trigger::RefreshButton;
                const uint32_t start = millis();
                while (held(b.refresh) && millis() - start < kLongPressMs) delay(20);
                wake.longPress = millis() - start >= kLongPressMs;
            }
            break;
        }
        default:  // power-on or reset: the config button may be held
            wake.config = held(b.config);
            break;
    }
    return wake;
}

// --- one cycle ------------------------------------------------------------------

void markRefreshed() {
    rtc.refresh = {true, now()};
    rtc.pendingRefresh = false;
}

void writeRow(void* context, uint16_t y, const uint8_t* row, uint16_t) {
    static_cast<Display*>(context)->writeRow(y, row);
}

bool show(const net::Buffer& png) {
    Display& d = display();
    const FrameError check = checkFrame(png.data, png.length, d.info());
    if (check != FrameError::None) {
        setError(std::string("frame rejected: ") + describe(check));
        return false;
    }
    if (!d.begin()) {
        setError("display init failed");
        return false;
    }
    const FrameError error = decodeFrame(png.data, png.length, d.info(), writeRow, &d);
    if (error != FrameError::None) {
        setError(std::string("frame rejected: ") + describe(error));
        return false;
    }
    const uint32_t start = millis();
    const bool ok = d.refresh();
    // Counted even when it reports an error: the waveform may have run anyway.
    markRefreshed();
    if (!ok) {
        setError("panel refresh failed");
        return false;
    }
    log_i("refreshed in %u ms", unsigned(millis() - start));
    return true;
}

[[noreturn]] void storage(const CyclePlan& plan) {
    if (plan.waitS) sleepFor(plan.waitS);  // the refresh limit applies here too
    if (display().begin()) {
        const PanelInfo& info = display().info();
        display().fill(nearestNative({255, 255, 255}, info.palette, info.paletteSize));
        display().refresh();
        markRefreshed();
    }
    rtc.storage = false;
    log_i("panel blanked for storage");
    sleepFor(0);
}

QueryParams frameParams(const Config& config) {
    const PanelInfo& info = display().info();
    QueryParams params{{"width", std::to_string(info.width)},
                       {"height", std::to_string(info.height)},
                       {"palette", info.paletteName}};
    const auto modes = splitModes(config.modes);
    if (!modes.empty()) params.push_back({"mode", modes[settings::modeIndex() % modes.size()]});
    params.push_back({"id", net::macId()});
    params.push_back({"fw", INKFETCH_VERSION});
    return params;
}

}  // namespace

void setup() {
    // Panel unpowered until it is actually needed (GPIO pads reset on wake-up).
    display().powerOff();
    Serial.begin(115200);
    loadRtcState();
    normalize(rtc.refresh, now());

    switch (esp_reset_reason()) {
        case ESP_RST_BROWNOUT:
            // Battery too low for Wi-Fi or a refresh: do not try again soon.
            setError("brownout: battery low");
            sleepFor(24 * 3600);
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            if (++rtc.crashes >= 3) {
                setError("repeated crashes, backing off");
                sleepFor(3600);
            }
            break;
        default:
            break;
    }

    const PanelInfo& panel = display().info();
    Config config = settings::load(panel.limits);
    const Wake wake = readWake();
    log_i("inkfetch %s (%s), wake trigger %d", INKFETCH_VERSION, INKFETCH_ENV, int(wake.trigger));

    if (wake.config || !config.complete() || !net::hasCredentials()) {
        const CyclePlan plan = planCycle(Trigger::PowerOn, rtc.refresh, now(), config.limits);
        const portal::Status status{rtc.refresh.everRefreshed ? ago(now() - rtc.refresh.lastRefreshAt) : "",
                                    rtc.lastError};
        const bool ready = portal::run(config, status, plan.waitS == 0);
        if (plan.waitS == 0) markRefreshed();  // the instructions were drawn
        if (ready) esp_restart();
        sleepFor(0);  // no usable configuration: wait for a button
    }

    if (wake.longPress) rtc.storage = true;
    if (wake.trigger == Trigger::ModeButton) {
        const auto modes = splitModes(config.modes);
        if (!modes.empty()) settings::setModeIndex((settings::modeIndex() + 1) % modes.size());
    }

    const Trigger trigger = rtc.pendingRefresh && wake.trigger == Trigger::Timer ? Trigger::RefreshButton : wake.trigger;
    const CyclePlan plan = planCycle(trigger, rtc.refresh, now(), config.limits);
    if (rtc.storage) storage(plan);
    if (plan.waitS) {
        // Too soon after the last refresh: come back when the panel allows it.
        if (trigger != Trigger::Timer) rtc.pendingRefresh = true;
        sleepFor(std::max(plan.waitS, kMinSleepS));
    }

    if (!net::connect(config.hostname, kWifiTimeoutMs)) {
        setError("Wi-Fi connection failed");
        if (ota::pendingVerify()) ota::rollback();
        sleepFor(nextSleep(config.limits, rtc.refresh, now(), config.pollS, std::nullopt));
    }

    const std::string url = withQuery(config.imageUrl, frameParams(config));
    log_i("GET %s%s", url.c_str(), plan.conditional ? "" : " (forced)");
    const net::Response response =
        net::get({url, config.token, plan.conditional ? rtc.etag : "", kMaxPngBytes});

    // A new firmware is confirmed once it reaches the image server at all.
    if (ota::pendingVerify()) response.reachedServer ? ota::confirm() : ota::rollback();

    if (response.status == 200 && response.body) {
        const uint32_t crc = esp_rom_crc32_le(0, response.body->data, response.body->length);
        if (plan.conditional && crc == rtc.crc) {
            log_i("unchanged (same content)");
            strlcpy(rtc.etag, response.etag.c_str(), sizeof rtc.etag);
        } else if (show(*response.body)) {
            rtc.crc = crc;
            strlcpy(rtc.etag, response.etag.c_str(), sizeof rtc.etag);
            setError("");
        }
    } else if (response.status == 304) {
        log_i("unchanged (304)");
    } else if (response.status == 401 || response.status == 403) {
        setError("server refused the token (HTTP " + std::to_string(response.status) + ")");
    } else {
        setError("GET failed: " + response.error);
    }

    if (!config.otaManifestUrl.empty() &&
        otaDue(wake.trigger, rtc.otaChecked ? std::optional<int64_t>(rtc.otaCheckedAt) : std::nullopt, now(),
               config.otaEveryHours)) {
        rtc.otaChecked = true;
        rtc.otaCheckedAt = now();
        if (ota::run(config.otaManifestUrl) == ota::Outcome::Updated) {
            display().powerOff();
            esp_restart();
        }
    }

    const auto retryAfter = config.honorRetryAfter ? response.retryAfter : std::nullopt;
    sleepFor(nextSleep(config.limits, rtc.refresh, now(), config.pollS, retryAfter));
}

void loop() {}
