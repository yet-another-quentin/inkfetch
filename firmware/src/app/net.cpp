#include "app/net.h"

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_wifi.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#include "inkfetch/http.h"

namespace inkfetch::net {

namespace {

constexpr uint16_t kHttpTimeoutMs = 30000;

// Minimal Stream writing into a fixed buffer, for HTTPClient::writeToStream (which
// handles both Content-Length and chunked responses).
class BufferStream : public Stream {
public:
    explicit BufferStream(Buffer& buffer) : _buffer(buffer) {}
    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t* data, size_t len) override {
        if (_buffer.length + len > _buffer.capacity) {
            overflow = true;
            return 0;
        }
        memcpy(_buffer.data + _buffer.length, data, len);
        _buffer.length += len;
        return len;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    bool overflow = false;

private:
    Buffer& _buffer;
};

}  // namespace

Buffer::~Buffer() { free(data); }

bool connect(const std::string& hostname, uint32_t timeoutMs) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(hostname.c_str());
    WiFi.begin();  // credentials stored by the portal
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeoutMs) {
            log_w("Wi-Fi: no connection after %u ms", timeoutMs);
            return false;
        }
        delay(100);
    }
    log_i("Wi-Fi: %s, RSSI %d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
}

bool hasCredentials() {
    WiFi.mode(WIFI_STA);
    wifi_config_t conf{};
    const bool stored = esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0] != 0;
#ifdef INKFETCH_DEV_WIFI_SSID
    if (!stored) {
        WiFi.persistent(true);
        WiFi.begin(INKFETCH_DEV_WIFI_SSID, INKFETCH_DEV_WIFI_PASSWORD);
        log_i("Wi-Fi: stored development credentials from secrets.h");
        return true;
    }
#endif
    return stored;
}

void disconnect() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

std::string macId() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char id[13];
    snprintf(id, sizeof id, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return id;
}

Response get(const Request& request) {
    Response response;
    std::unique_ptr<NetworkClient> client;
    if (isHttpsUrl(request.url)) {
        auto secure = std::make_unique<NetworkClientSecure>();
        secure->useBuiltinCACertBundle();
        client = std::move(secure);
    } else {
        client = std::make_unique<NetworkClient>();
    }

    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    // A redirect to another host must not reuse the first connection.
    http.setReuse(false);
    http.setFollowRedirects(request.bearerToken.empty() ? HTTPC_STRICT_FOLLOW_REDIRECTS
                                                        : HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(*client, request.url.c_str())) {
        response.error = "invalid URL";
        return response;
    }
    const char* headers[] = {"ETag", "Retry-After"};
    http.collectHeaders(headers, 2);
    if (!request.bearerToken.empty()) http.addHeader("Authorization", ("Bearer " + request.bearerToken).c_str());
    if (!request.ifNoneMatch.empty()) http.addHeader("If-None-Match", request.ifNoneMatch.c_str());

    response.status = http.GET();
    if (response.status < 0) {
        response.error = http.errorToString(response.status).c_str();
        return response;
    }
    response.reachedServer = true;
    response.etag = http.header("ETag").c_str();
    response.retryAfter = parseRetryAfter(http.header("Retry-After").c_str());
    if (response.status != HTTP_CODE_OK) {
        response.error = "HTTP " + std::to_string(response.status);
        return response;
    }

    const int size = http.getSize();  // -1 when unknown (chunked)
    if (size > int(request.maxBytes)) {
        response.error = "response too large (" + std::to_string(size) + " bytes)";
        response.status = -1;
        return response;
    }
    auto body = std::make_unique<Buffer>();
    body->capacity = size > 0 ? size_t(size) : request.maxBytes;
    body->data = static_cast<uint8_t*>(ps_malloc(body->capacity));
    if (!body->data) {
        response.error = "out of memory";
        response.status = -1;
        return response;
    }
    BufferStream out(*body);
    const int written = http.writeToStream(&out);
    if (written < 0 || out.overflow || (size > 0 && body->length != size_t(size))) {
        response.error = out.overflow ? "response too large" : "download interrupted";
        response.status = -1;
        return response;
    }
    response.body = std::move(body);
    return response;
}

}  // namespace inkfetch::net
