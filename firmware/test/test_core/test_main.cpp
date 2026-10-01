// Native tests for lib/inkfetch_core: `pio test -e native`, no hardware needed.
#include <unity.h>

#include <cstdio>
#include <string>
#include <vector>

#include "inkfetch/config.h"
#include "inkfetch/http.h"
#include "inkfetch/ota.h"
#include "inkfetch/panel.h"
#include "inkfetch/png_frame.h"
#include "inkfetch/policy.h"

using namespace inkfetch;

#ifndef INKFETCH_FIXTURES
#error "INKFETCH_FIXTURES must point at the repository's fixtures/ directory"
#endif

namespace {

const Rgb kSpectra6[] = {{0, 0, 0}, {255, 255, 255}, {255, 255, 0}, {255, 0, 0}, {0, 0, 255}, {0, 255, 0}};
const Limits kLimits{180, 86400};
const PanelInfo kFixturePanel{64, 48, "spectra6", kSpectra6, 6, kLimits};

std::vector<uint8_t> readFixture(const char* name) {
    const std::string path = std::string(INKFETCH_FIXTURES) + "/" + name;
    std::vector<uint8_t> data;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        uint8_t buffer[4096];
        size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) data.insert(data.end(), buffer, buffer + n);
        std::fclose(f);
    }
    TEST_ASSERT_FALSE_MESSAGE(data.empty(), path.c_str());
    return data;
}

struct Collected {
    std::vector<uint8_t> pixels;
    int rows = 0;
    int lastY = -1;
    bool ordered = true;
};

void collect(void* ctx, uint16_t y, const uint8_t* row, uint16_t width) {
    auto* c = static_cast<Collected*>(ctx);
    c->ordered = c->ordered && int(y) == c->lastY + 1;
    c->lastY = y;
    c->pixels.insert(c->pixels.end(), row, row + width);
    ++c->rows;
}

// --- palette ------------------------------------------------------------------

void test_nearest_native_pure_and_calibrated() {
    for (size_t i = 0; i < 6; ++i) TEST_ASSERT_EQUAL(i, nearestNative(kSpectra6[i], kSpectra6, 6));
    // fugleramme's calibrated Spectra 6 palette (tools/src/inkfetch/palettes.py).
    const Rgb calibrated[] = {{0, 0, 0}, {208, 209, 210}, {231, 222, 35}, {205, 36, 37}, {30, 29, 174}, {29, 173, 35}};
    for (size_t i = 0; i < 6; ++i) TEST_ASSERT_EQUAL(i, nearestNative(calibrated[i], kSpectra6, 6));
}

void test_nearest_native_lowest_index_on_tie() {
    const Rgb two[] = {{0, 0, 0}, {254, 254, 254}};
    TEST_ASSERT_EQUAL(0, nearestNative({127, 127, 127}, two, 2));
}

// --- PNG frames ---------------------------------------------------------------

void expectFixture(const char* png, const char* native) {
    const auto data = readFixture(png);
    const auto expected = readFixture(native);
    TEST_ASSERT_EQUAL(int(FrameError::None), int(checkFrame(data.data(), data.size(), kFixturePanel)));
    Collected c;
    TEST_ASSERT_EQUAL(int(FrameError::None), int(decodeFrame(data.data(), data.size(), kFixturePanel, collect, &c)));
    TEST_ASSERT_EQUAL(48, c.rows);
    TEST_ASSERT_TRUE(c.ordered);
    TEST_ASSERT_EQUAL(expected.size(), c.pixels.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), c.pixels.data(), expected.size());
}

void test_decode_4bit_calibrated() { expectFixture("spectra6-calibrated-4bit.png", "spectra6-calibrated-4bit.native"); }
void test_decode_1bit() { expectFixture("bw-1bit.png", "bw-1bit.native"); }
void test_decode_2bit() { expectFixture("four-2bit.png", "four-2bit.native"); }
void test_decode_8bit() { expectFixture("many-8bit.png", "many-8bit.native"); }

void test_reject_wrong_size() {
    const auto data = readFixture("bw-1bit.png");
    const PanelInfo other{1600, 1200, "spectra6", kSpectra6, 6, kLimits};
    TEST_ASSERT_EQUAL(int(FrameError::WrongSize), int(checkFrame(data.data(), data.size(), other)));
}

void test_reject_truecolor() {
    const auto data = readFixture("truecolor.png");
    TEST_ASSERT_EQUAL(int(FrameError::NotIndexed), int(checkFrame(data.data(), data.size(), kFixturePanel)));
}

void test_reject_truncated() {
    const auto data = readFixture("truncated.png");
    Collected c;
    TEST_ASSERT_EQUAL(int(FrameError::Corrupt), int(decodeFrame(data.data(), data.size(), kFixturePanel, collect, &c)));
}

void test_reject_garbage() {
    const uint8_t junk[] = "definitely not a png";
    TEST_ASSERT_EQUAL(int(FrameError::NotPng), int(checkFrame(junk, sizeof junk, kFixturePanel)));
    TEST_ASSERT_EQUAL(int(FrameError::NotPng), int(checkFrame(junk, 3, kFixturePanel)));
}

// --- HTTP ---------------------------------------------------------------------

void test_with_query() {
    const QueryParams p{{"width", "1600"}, {"palette", "spectra6"}, {"mode", ""}, {"id", "a b&c"}};
    TEST_ASSERT_EQUAL_STRING("http://h/frame.png?width=1600&palette=spectra6&id=a%20b%26c",
                             withQuery("http://h/frame.png", p).c_str());
    TEST_ASSERT_EQUAL_STRING("http://h/f?x=1&width=1600&palette=spectra6&id=a%20b%26c",
                             withQuery("http://h/f?x=1", p).c_str());
    TEST_ASSERT_EQUAL_STRING("http://h/f?width=1#top", withQuery("http://h/f?#top", {{"width", "1"}}).c_str());
    TEST_ASSERT_EQUAL_STRING("http://h/f", withQuery("http://h/f", {}).c_str());
}

void test_retry_after() {
    TEST_ASSERT_FALSE(parseRetryAfter(nullptr).has_value());
    TEST_ASSERT_FALSE(parseRetryAfter("").has_value());
    TEST_ASSERT_EQUAL_UINT32(900, *parseRetryAfter("900"));
    TEST_ASSERT_EQUAL_UINT32(0, *parseRetryAfter("0"));
    TEST_ASSERT_EQUAL_UINT32(120, *parseRetryAfter("  120 "));
    TEST_ASSERT_FALSE(parseRetryAfter("Wed, 21 Oct 2026 07:28:00 GMT").has_value());
    TEST_ASSERT_FALSE(parseRetryAfter("-5").has_value());
    TEST_ASSERT_FALSE(parseRetryAfter("12abc").has_value());
    TEST_ASSERT_FALSE(parseRetryAfter("99999999999").has_value());
}

void test_url_schemes() {
    TEST_ASSERT_TRUE(isHttpUrl("http://h/x"));
    TEST_ASSERT_TRUE(isHttpUrl("https://h/x"));
    TEST_ASSERT_TRUE(isHttpsUrl("https://h/x"));
    TEST_ASSERT_FALSE(isHttpsUrl("http://h/x"));
    TEST_ASSERT_FALSE(isHttpUrl("ftp://h/x"));
    TEST_ASSERT_FALSE(isHttpUrl("http://"));
}

// --- OTA ----------------------------------------------------------------------

const char* kHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::string manifest(const std::string& env, const std::string& version, const std::string& url = "https://x/fw.bin",
                     const std::string& hash = kHash) {
    return "{\"env\":\"" + env + "\",\"version\":\"" + version + "\",\"url\":\"" + url + "\",\"sha256\":\"" + hash + "\"}";
}

void test_versions() {
    TEST_ASSERT_TRUE(parseVersion("1.2.3").has_value());
    TEST_ASSERT_TRUE(parseVersion("v10.0.12").has_value());
    TEST_ASSERT_FALSE(parseVersion("1.2").has_value());
    TEST_ASSERT_FALSE(parseVersion("1.2.3-rc1").has_value());
    TEST_ASSERT_FALSE(parseVersion("dev").has_value());
    const Version current = *parseVersion("1.2.3");
    TEST_ASSERT_TRUE(isNewer(*parseVersion("1.2.4"), current));
    TEST_ASSERT_TRUE(isNewer(*parseVersion("1.10.0"), current));
    TEST_ASSERT_TRUE(isNewer(*parseVersion("2.0.0"), current));
    TEST_ASSERT_FALSE(isNewer(*parseVersion("1.2.3"), current));
    TEST_ASSERT_FALSE(isNewer(*parseVersion("1.1.9"), current));
}

void test_manifest() {
    const Version current{1, 2, 3};
    Manifest m;
    std::string json = manifest("seeed-ee02", "1.3.0");
    TEST_ASSERT_EQUAL(int(ManifestError::None), int(parseManifest(json.c_str(), json.size(), m)));
    TEST_ASSERT_EQUAL_STRING("https://x/fw.bin", m.url.c_str());
    TEST_ASSERT_TRUE(shouldUpdate(m, "seeed-ee02", current));
    TEST_ASSERT_FALSE(shouldUpdate(m, "other-board", current));

    json = manifest("seeed-ee02", "1.2.3");
    parseManifest(json.c_str(), json.size(), m);
    TEST_ASSERT_FALSE(shouldUpdate(m, "seeed-ee02", current));
    json = manifest("seeed-ee02", "1.0.0");
    parseManifest(json.c_str(), json.size(), m);
    TEST_ASSERT_FALSE(shouldUpdate(m, "seeed-ee02", current));
}

void test_manifest_errors() {
    Manifest m;
    auto parse = [&](const std::string& json) { return int(parseManifest(json.c_str(), json.size(), m)); };
    TEST_ASSERT_EQUAL(int(ManifestError::InvalidJson), parse("{nope"));
    TEST_ASSERT_EQUAL(int(ManifestError::InvalidJson), parse("[1,2]"));
    TEST_ASSERT_EQUAL(int(ManifestError::MissingField), parse("{\"env\":\"x\"}"));
    TEST_ASSERT_EQUAL(int(ManifestError::BadVersion), parse(manifest("x", "latest")));
    TEST_ASSERT_EQUAL(int(ManifestError::BadUrl), parse(manifest("x", "1.0.0", "file:///fw.bin")));
    TEST_ASSERT_EQUAL(int(ManifestError::BadHash), parse(manifest("x", "1.0.0", "https://x/fw", "ABC")));
}

// --- panel protection -----------------------------------------------------------

void test_first_cycle_refreshes() {
    const CyclePlan p = planCycle(Trigger::PowerOn, RefreshState{}, 1000, kLimits);
    TEST_ASSERT_FALSE(p.conditional);
    TEST_ASSERT_TRUE(p.maintenance);
    TEST_ASSERT_EQUAL_UINT32(0, p.waitS);
}

void test_timer_cycle_is_conditional() {
    const CyclePlan p = planCycle(Trigger::Timer, RefreshState{true, 0}, 900, kLimits);
    TEST_ASSERT_TRUE(p.conditional);
    TEST_ASSERT_FALSE(p.maintenance);
    TEST_ASSERT_EQUAL_UINT32(0, p.waitS);
}

void test_maintenance_after_max_stale() {
    const CyclePlan p = planCycle(Trigger::Timer, RefreshState{true, 0}, 86400, kLimits);
    TEST_ASSERT_FALSE(p.conditional);
    TEST_ASSERT_TRUE(p.maintenance);
    TEST_ASSERT_TRUE(planCycle(Trigger::Timer, RefreshState{true, 0}, 86399, kLimits).conditional);
}

void test_too_soon_waits_even_for_buttons() {
    const RefreshState justRefreshed{true, 1000};
    for (Trigger t : {Trigger::Timer, Trigger::RefreshButton, Trigger::ModeButton, Trigger::PowerOn}) {
        TEST_ASSERT_EQUAL_UINT32(120, planCycle(t, justRefreshed, 1060, kLimits).waitS);
    }
    TEST_ASSERT_EQUAL_UINT32(0, planCycle(Trigger::RefreshButton, justRefreshed, 1180, kLimits).waitS);
    TEST_ASSERT_FALSE(planCycle(Trigger::RefreshButton, justRefreshed, 1180, kLimits).conditional);
}

void test_clock_going_backwards_waits() {
    // Time since the last refresh is unknown: never refresh right away.
    TEST_ASSERT_EQUAL_UINT32(180, planCycle(Trigger::Timer, RefreshState{true, 5000}, 100, kLimits).waitS);
    RefreshState state{true, 5000};
    normalize(state, 100);
    TEST_ASSERT_EQUAL(100, state.lastRefreshAt);
    TEST_ASSERT_EQUAL_UINT32(180, planCycle(Trigger::RefreshButton, state, 100, kLimits).waitS);
    TEST_ASSERT_EQUAL_UINT32(0, planCycle(Trigger::RefreshButton, state, 280, kLimits).waitS);
    RefreshState never{};
    normalize(never, 100);
    TEST_ASSERT_FALSE(never.everRefreshed);
}

void test_next_sleep() {
    const RefreshState fresh{true, 0};
    TEST_ASSERT_EQUAL_UINT32(900, nextSleep(kLimits, fresh, 0, 900, std::nullopt));
    TEST_ASSERT_EQUAL_UINT32(3600, nextSleep(kLimits, fresh, 0, 900, 3600u));
    TEST_ASSERT_EQUAL_UINT32(180, nextSleep(kLimits, fresh, 0, 900, 10u));        // clamped up
    TEST_ASSERT_EQUAL_UINT32(86400, nextSleep(kLimits, fresh, 0, 900, 999999u));  // clamped down
    // Never sleep past the maintenance refresh.
    TEST_ASSERT_EQUAL_UINT32(400, nextSleep(kLimits, fresh, 86000, 900, std::nullopt));
    TEST_ASSERT_EQUAL_UINT32(400, nextSleep(kLimits, fresh, 86000, 900, 30000u));
    // Overdue (the maintenance attempt failed): back to the normal interval, no tight loop.
    TEST_ASSERT_EQUAL_UINT32(900, nextSleep(kLimits, fresh, 90000, 900, std::nullopt));
    TEST_ASSERT_EQUAL_UINT32(3600, nextSleep(kLimits, fresh, 90000, 900, 3600u));
    TEST_ASSERT_EQUAL_UINT32(kMinSleepS, nextSleep(kLimits, fresh, 0, 5, std::nullopt));
    // Never refreshed: plain interval.
    TEST_ASSERT_EQUAL_UINT32(900, nextSleep(kLimits, RefreshState{}, 0, 900, std::nullopt));
}

void test_ota_due() {
    TEST_ASSERT_TRUE(otaDue(Trigger::Timer, std::nullopt, 0, 24));
    TEST_ASSERT_TRUE(otaDue(Trigger::PowerOn, std::nullopt, 0, 24));
    TEST_ASSERT_FALSE(otaDue(Trigger::RefreshButton, std::nullopt, 0, 24));
    TEST_ASSERT_FALSE(otaDue(Trigger::ModeButton, std::nullopt, 0, 24));
    TEST_ASSERT_FALSE(otaDue(Trigger::Timer, int64_t(0), 3600, 24));
    TEST_ASSERT_TRUE(otaDue(Trigger::Timer, int64_t(0), 24 * 3600, 24));
}

// --- configuration ------------------------------------------------------------

ConfigForm validForm() {
    ConfigForm f;
    f.hostname = "inkfetch";
    f.imageUrl = "http://server:8080/frame.png";
    f.modes = "collage,latest,arrival";
    f.pollS = "900";
    f.otaManifestUrl = "";
    f.otaEveryHours = "24";
    f.minRefreshS = "180";
    f.maxStaleS = "86400";
    return f;
}

void test_config_valid() {
    Config out;
    const auto errors = validate(validForm(), kLimits, Config{}, out);
    TEST_ASSERT_EQUAL(0, errors.size());
    TEST_ASSERT_EQUAL_STRING("http://server:8080/frame.png", out.imageUrl.c_str());
    TEST_ASSERT_EQUAL_UINT32(900, out.pollS);
    TEST_ASSERT_EQUAL_UINT32(180, out.limits.minRefreshS);
    TEST_ASSERT_TRUE(out.complete());
}

void test_config_protection_only_more_cautious() {
    Config out;
    ConfigForm f = validForm();
    f.minRefreshS = "60";  // less cautious than the panel
    TEST_ASSERT_EQUAL(1, validate(f, kLimits, Config{}, out).size());
    f = validForm();
    f.maxStaleS = "172800";
    TEST_ASSERT_EQUAL(1, validate(f, kLimits, Config{}, out).size());
    f = validForm();
    f.minRefreshS = "600";  // more cautious: fine
    f.maxStaleS = "43200";
    TEST_ASSERT_EQUAL(0, validate(f, kLimits, Config{}, out).size());
    TEST_ASSERT_EQUAL_UINT32(600, out.limits.minRefreshS);
    TEST_ASSERT_EQUAL_UINT32(43200, out.limits.maxStaleS);
    f.minRefreshS = "50000";  // min above max
    TEST_ASSERT_EQUAL(1, validate(f, kLimits, Config{}, out).size());
}

void test_config_errors_leave_output_untouched() {
    Config out;
    out.imageUrl = "http://kept/";
    ConfigForm f = validForm();
    f.imageUrl = "server/frame.png";
    f.hostname = "Bad Host";
    f.pollS = "10";
    f.otaManifestUrl = "ftp://x";
    f.otaEveryHours = "abc";
    f.modes = "a b";
    TEST_ASSERT_EQUAL(6, validate(f, kLimits, Config{}, out).size());
    TEST_ASSERT_EQUAL_STRING("http://kept/", out.imageUrl.c_str());
}

void test_config_token_kept_replaced_cleared() {
    Config previous;
    previous.token = "old";
    Config out;
    ConfigForm f = validForm();
    validate(f, kLimits, previous, out);
    TEST_ASSERT_EQUAL_STRING("old", out.token.c_str());
    f.token = "new";
    validate(f, kLimits, previous, out);
    TEST_ASSERT_EQUAL_STRING("new", out.token.c_str());
    f.token = "";
    f.clearToken = true;
    validate(f, kLimits, previous, out);
    TEST_ASSERT_EQUAL_STRING("", out.token.c_str());
    TEST_ASSERT_EQUAL_STRING("", toForm(previous).token.c_str());  // never echoed
}

void test_split_modes() {
    const auto modes = splitModes(" collage, latest ,,arrival ");
    TEST_ASSERT_EQUAL(3, modes.size());
    TEST_ASSERT_EQUAL_STRING("latest", modes[1].c_str());
    TEST_ASSERT_EQUAL(0, splitModes("").size());
}

void test_wifi_qr_payload() {
    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:inkfetch-ab12;P:pa\\;ss\\:w\\\\d;;",
                             wifiQrPayload("inkfetch-ab12", "pa;ss:w\\d").c_str());
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_nearest_native_pure_and_calibrated);
    RUN_TEST(test_nearest_native_lowest_index_on_tie);
    RUN_TEST(test_decode_4bit_calibrated);
    RUN_TEST(test_decode_1bit);
    RUN_TEST(test_decode_2bit);
    RUN_TEST(test_decode_8bit);
    RUN_TEST(test_reject_wrong_size);
    RUN_TEST(test_reject_truecolor);
    RUN_TEST(test_reject_truncated);
    RUN_TEST(test_reject_garbage);
    RUN_TEST(test_with_query);
    RUN_TEST(test_retry_after);
    RUN_TEST(test_url_schemes);
    RUN_TEST(test_versions);
    RUN_TEST(test_manifest);
    RUN_TEST(test_manifest_errors);
    RUN_TEST(test_first_cycle_refreshes);
    RUN_TEST(test_timer_cycle_is_conditional);
    RUN_TEST(test_maintenance_after_max_stale);
    RUN_TEST(test_too_soon_waits_even_for_buttons);
    RUN_TEST(test_clock_going_backwards_waits);
    RUN_TEST(test_next_sleep);
    RUN_TEST(test_ota_due);
    RUN_TEST(test_config_valid);
    RUN_TEST(test_config_protection_only_more_cautious);
    RUN_TEST(test_config_errors_leave_output_untouched);
    RUN_TEST(test_config_token_kept_replaced_cleared);
    RUN_TEST(test_split_modes);
    RUN_TEST(test_wifi_qr_payload);
    return UNITY_END();
}
