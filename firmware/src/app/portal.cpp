#include "app/portal.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include <cstring>
#include <memory>
#include <vector>

#include "app/net.h"
#include "app/settings.h"
#include "profile.h"

namespace inkfetch::portal {

namespace {

constexpr uint32_t kTimeoutS = 600;

constexpr uint32_t kGraceAfterSaveMs = 60000;

// WiFiManager renders these buffers on every page load; we rewrite them in place.
char statusHtml[2048];
char retryAfterAttrs[64];  // the checkbox's "checked" state follows the saved setting

std::string htmlEscape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

void renderStatus(const Config& config, const Status& status, const std::vector<std::string>& errors,
                  bool saved) {
    std::string html = "<div class='msg'><b>inkfetch " INKFETCH_VERSION "</b> (" INKFETCH_ENV ")<br>";
    html += "Last refresh: " + htmlEscape(status.lastRefresh.empty() ? "never" : status.lastRefresh) + "<br>";
    if (!status.lastError.empty()) html += "Last error: " + htmlEscape(status.lastError) + "<br>";
    html += std::string("Image URL: ") + (config.imageUrl.empty() ? "<i>not set</i>" : htmlEscape(config.imageUrl));
    html += "</div>";
    if (saved) html += "<div class='msg S'>Settings saved.</div>";
    if (!errors.empty()) {
        html += "<div class='msg D'><b>Not saved:</b><ul>";
        for (const auto& e : errors) html += "<li>" + htmlEscape(e) + "</li>";
        html += "</ul></div>";
    }
    if (saved) {
        html += "<p>The frame restarts in a minute. Use <b>Configure WiFi</b> now to change the network.</p>";
    } else {
        html += "<p>First <b>Setup</b>, then <b>Configure WiFi</b>.</p>";
    }
    strlcpy(statusHtml, html.c_str(), sizeof statusHtml);
}

// A text field whose value outlives the WiFiManagerParameter that points at it.
struct Field {
    std::string value;
    std::unique_ptr<WiFiManagerParameter> param;
};

}  // namespace

bool run(Config& config, const Status& status, bool showOnPanel) {
    const std::string ssid = "inkfetch-" + net::macId().substr(8);
    const std::string password = settings::portalPassword();
    const Limits panelLimits = display().info().limits;

    if (showOnPanel) {
        Message m;
        m.title = "inkfetch setup";
        m.lines = {"1. Join the Wi-Fi network:", "   " + ssid, "   password: " + password,
                   "2. Open http://192.168.4.1", "3. Setup, then Configure WiFi", "",
                   "Closes after " + std::to_string(kTimeoutS / 60) + " minutes."};
        m.qr = wifiQrPayload(ssid, password);
        if (display().begin()) {
            display().drawMessage(m);
            display().refresh();
        }
        display().powerOff();
    }

    const ConfigForm form = toForm(config);
    auto setRetryAttrs = [](bool checked) {
        strlcpy(retryAfterAttrs, checked ? "type='checkbox' checked" : "type='checkbox'", sizeof retryAfterAttrs);
    };
    setRetryAttrs(form.honorRetryAfter);
    std::vector<Field> fields;
    fields.reserve(12);
    auto text = [&](const char* id, const char* label, const std::string& value, int length,
                    const char* custom = "") {
        fields.push_back({value, nullptr});
        fields.back().param = std::make_unique<WiFiManagerParameter>(id, label, fields.back().value.c_str(), length, custom);
    };
    text("imageUrl", "Image URL (http:// or https://)", form.imageUrl, 256);
    text("token", "Bearer token (empty: keep the stored one)", "", 128, "type='password' autocomplete='off'");
    // Checkboxes: WiFiManager renders value='<current value>' before our attributes,
    // and stores "" for an unchecked box, so the value is reset to "1" after each save.
    fields.push_back({"", std::make_unique<WiFiManagerParameter>("clearToken", "Clear the stored token", "1", 2,
                                                                  "type='checkbox'", WFM_LABEL_AFTER)});
    text("modes", "Modes for the mode button (comma-separated, optional)", form.modes, 128);
    text("pollS", "Poll interval, seconds", form.pollS, 8, "type='number' min='60' max='86400'");
    fields.push_back({"", std::make_unique<WiFiManagerParameter>("retryAfter", "Follow the server's Retry-After", "1",
                                                                  2, retryAfterAttrs, WFM_LABEL_AFTER)});
    text("otaUrl", "OTA manifest URL (empty: no updates)", form.otaManifestUrl, 256);
    text("otaHours", "OTA check, every N hours", form.otaEveryHours, 5, "type='number' min='1' max='720'");
    const std::string minLabel = "Minimum time between refreshes, seconds (panel: at least " +
                                 std::to_string(panelLimits.minRefreshS) + ")";
    const std::string maxLabel = "Maximum time without refresh, seconds (panel: at most " +
                                 std::to_string(panelLimits.maxStaleS) + ")";
    text("minRefresh", minLabel.c_str(), form.minRefreshS, 8, "type='number'");
    text("maxStale", maxLabel.c_str(), form.maxStaleS, 8, "type='number'");
    text("hostname", "Hostname", form.hostname, 33);

    WiFiManager wm;
    for (auto& f : fields) wm.addParameter(f.param.get());
    std::vector<const char*> menu = {"param", "wifi", "custom", "info", "exit"};
    wm.setMenu(menu);
    wm.setTitle("inkfetch");
    wm.setHostname(config.hostname.c_str());
    wm.setParamsPage(true);
    wm.setShowInfoUpdate(false);  // updates go through the OTA manifest
    wm.setDisableConfigPortal(false);  // stay open after a successful Wi-Fi save
    wm.setConfigPortalBlocking(false);
    renderStatus(config, status, {}, false);
    wm.setCustomMenuHTML(statusHtml);

    auto param = [&](const char* id) -> WiFiManagerParameter* {
        for (const auto& f : fields) {
            if (strcmp(f.param->getID(), id) == 0) return f.param.get();
        }
        return nullptr;
    };
    auto value = [&](const char* id) -> std::string { return param(id)->getValue(); };
    bool valid = config.complete();
    bool changed = false;  // something was saved in this session
    uint32_t exitAt = 0;   // after a settings save, leave time to change the Wi-Fi too
    wm.setSaveParamsCallback([&]() {
        ConfigForm submitted;
        submitted.imageUrl = value("imageUrl");
        submitted.token = value("token");
        submitted.clearToken = value("clearToken") == "1";
        submitted.modes = value("modes");
        submitted.pollS = value("pollS");
        submitted.honorRetryAfter = value("retryAfter") == "1";
        submitted.otaManifestUrl = value("otaUrl");
        submitted.otaEveryHours = value("otaHours");
        submitted.minRefreshS = value("minRefresh");
        submitted.maxStaleS = value("maxStale");
        submitted.hostname = value("hostname");
        const auto errors = validate(submitted, panelLimits, config, config);
        if (errors.empty()) {
            settings::save(config);
            valid = changed = true;
            if (wm.getWiFiIsSaved()) exitAt = millis() + kGraceAfterSaveMs;
        }
        param("clearToken")->setValue("1", 2);
        param("retryAfter")->setValue("1", 2);
        param("token")->setValue("", 128);
        setRetryAttrs(config.honorRetryAfter);
        renderStatus(config, status, errors, errors.empty());
    });
    wm.setSaveConfigCallback([&]() { changed = true; });

    WiFi.mode(WIFI_AP_STA);
    wm.setConfigPortalTimeout(kTimeoutS);
    wm.startConfigPortal(ssid.c_str(), password.c_str());
    log_i("portal: join %s (password %s), open http://192.168.4.1", ssid.c_str(), password.c_str());

    const uint32_t deadline = millis() + kTimeoutS * 1000;
    while (wm.getConfigPortalActive() && int32_t(deadline - millis()) > 0) {
        wm.process();
        // Done once the settings are valid and the frame is on the Wi-Fi.
        if (changed && valid && WiFi.status() == WL_CONNECTED) {
            delay(2000);  // let the "saved" page reach the browser
            break;
        }
        if (exitAt && int32_t(millis() - exitAt) > 0) break;
        delay(5);
    }
    wm.stopConfigPortal();
    return valid && wm.getWiFiIsSaved();
}

}  // namespace inkfetch::portal
