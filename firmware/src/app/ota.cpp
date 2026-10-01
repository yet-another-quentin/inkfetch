#include "app/ota.h"

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include <memory>

#include "app/net.h"
#include "inkfetch/http.h"
#include "inkfetch/ota.h"

// Confirm new images ourselves, after a successful cycle (see ota.h), instead of
// letting the Arduino core confirm them at boot.
extern "C" bool verifyRollbackLater() { return true; }

namespace inkfetch::ota {

namespace {

constexpr size_t kMaxManifestBytes = 4096;
constexpr const char* kNamespace = "inkfetch-ota";

std::string toHex(const uint8_t* bytes, size_t n) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        out += hex[bytes[i] >> 4];
        out += hex[bytes[i] & 0x0F];
    }
    return out;
}

std::string versionString(const Version& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}

std::string readKey(const char* key) {
    Preferences p;
    if (!p.begin(kNamespace, true)) return "";
    const std::string value = p.getString(key, "").c_str();
    p.end();
    return value;
}

void writeKey(const char* key, const std::string& value) {
    Preferences p;
    p.begin(kNamespace, false);
    p.putString(key, value.c_str());
    p.end();
}

// Remembers which version was being tried, so a rollback (explicit, or by the
// bootloader after a crash) marks it as rejected: it is never installed again.
void noteRejectedVersion() {
    const std::string tried = readKey("tried");
    if (tried.empty() || tried == INKFETCH_VERSION || pendingVerify()) return;
    log_w("OTA: %s was rolled back, it will not be installed again", tried.c_str());
    writeKey("rejected", tried);
    writeKey("tried", "");
}

// Feeds the download to the updater and the hash; used with writeToStream, which
// also decodes chunked responses.
class UpdateStream : public Stream {
public:
    UpdateStream() {
        mbedtls_sha256_init(&_sha);
        mbedtls_sha256_starts(&_sha, 0);
    }
    ~UpdateStream() override { mbedtls_sha256_free(&_sha); }
    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t* data, size_t len) override {
        mbedtls_sha256_update(&_sha, data, len);
        const size_t written = Update.write(const_cast<uint8_t*>(data), len);
        total += written;
        return written;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    std::string digest() {
        uint8_t out[32];
        mbedtls_sha256_finish(&_sha, out);
        return toHex(out, sizeof out);
    }
    size_t total = 0;

private:
    mbedtls_sha256_context _sha;
};

bool install(const Manifest& manifest) {
    std::unique_ptr<NetworkClient> client;
    if (isHttpsUrl(manifest.url)) {
        auto secure = std::make_unique<NetworkClientSecure>();
        secure->useBuiltinCACertBundle();
        client = std::move(secure);
    } else {
        client = std::make_unique<NetworkClient>();
    }
    HTTPClient http;
    http.setTimeout(30000);
    http.setReuse(false);  // GitHub release assets redirect to another host
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(*client, manifest.url.c_str()) || http.GET() != HTTP_CODE_OK) {
        log_e("OTA: cannot download %s", manifest.url.c_str());
        return false;
    }
    const int size = http.getSize();
    if (!Update.begin(size > 0 ? size_t(size) : UPDATE_SIZE_UNKNOWN)) {
        log_e("OTA: %s", Update.errorString());
        return false;
    }
    UpdateStream out;
    const int written = http.writeToStream(&out);
    const std::string digest = out.digest();
    if (written < 0 || (size > 0 && out.total != size_t(size)) || digest != manifest.sha256) {
        log_e("OTA: download incomplete or SHA-256 mismatch (%u bytes)", unsigned(out.total));
        Update.abort();
        return false;
    }
    if (!Update.end(true)) {
        log_e("OTA: %s", Update.errorString());
        return false;
    }
    writeKey("tried", versionString(manifest.version));
    log_i("OTA: installed %s, restarting", versionString(manifest.version).c_str());
    return true;
}

}  // namespace

Outcome run(const std::string& manifestUrl) {
    noteRejectedVersion();
    const auto current = parseVersion(INKFETCH_VERSION);
    if (!current) {
        log_w("OTA: build version %s is not x.y.z, skipping", INKFETCH_VERSION);
        return Outcome::UpToDate;
    }
    const net::Response response = net::get({manifestUrl, "", "", kMaxManifestBytes});
    if (!response.body) {
        log_w("OTA: manifest %s: %s", manifestUrl.c_str(), response.error.c_str());
        return Outcome::Failed;
    }
    Manifest manifest;
    const ManifestError error =
        parseManifest(reinterpret_cast<const char*>(response.body->data), response.body->length, manifest);
    if (error != ManifestError::None) {
        log_w("OTA: %s", describe(error));
        return Outcome::Failed;
    }
    if (!shouldUpdate(manifest, INKFETCH_ENV, *current)) return Outcome::UpToDate;
    if (versionString(manifest.version) == readKey("rejected")) {
        log_i("OTA: %s was rolled back before, waiting for a newer version", versionString(manifest.version).c_str());
        return Outcome::UpToDate;
    }
    return install(manifest) ? Outcome::Updated : Outcome::Failed;
}

bool pendingVerify() {
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

void confirm() {
    if (pendingVerify()) {
        esp_ota_mark_app_valid_cancel_rollback();
        writeKey("tried", "");
        log_i("OTA: new firmware confirmed");
    }
}

void rollback() {
    log_e("OTA: new firmware could not reach the server, rolling back");
    writeKey("rejected", INKFETCH_VERSION);
    writeKey("tried", "");
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart();  // only reached when there is nothing to roll back to
}

}  // namespace inkfetch::ota
