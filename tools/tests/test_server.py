import io
import json
import threading
import urllib.error
import urllib.request
from datetime import datetime, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest
from PIL import Image

from inkfetch import manifest
from inkfetch.server import (
    UNAVAILABLE_RETRY_S,
    FileSource,
    QuietHours,
    ServerOptions,
    UrlSource,
    make_server,
)


def png_bytes(colour=(255, 0, 0), size=(40, 30)) -> bytes:
    buffer = io.BytesIO()
    Image.new("RGB", size, colour).save(buffer, format="PNG")
    return buffer.getvalue()


def start(server: ThreadingHTTPServer) -> str:
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return f"http://127.0.0.1:{server.server_address[1]}"


def get(url: str, headers: dict[str, str] | None = None):
    """(status, headers, body) without raising on HTTP errors."""
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers or {}), timeout=10) as resp:
            return resp.status, resp.headers, resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.headers, exc.read()


@pytest.fixture
def picture(tmp_path: Path) -> Path:
    path = tmp_path / "picture.png"
    path.write_bytes(png_bytes())
    return path


@pytest.fixture
def serve():
    servers: list[ThreadingHTTPServer] = []

    def _serve(source, **options) -> str:
        server = make_server(source, ServerOptions(**options), "127.0.0.1", 0)
        servers.append(server)
        return start(server)

    yield _serve
    for server in servers:
        server.shutdown()
        server.server_close()


def test_frame_has_requested_size_and_etag_then_304(picture, serve):
    base = serve(FileSource(picture))
    status, headers, body = get(f"{base}/frame.png?width=320&height=240&palette=bw")
    assert status == 200
    assert headers["Content-Type"] == "image/png"
    assert headers["Cache-Control"] == "no-cache"
    img = Image.open(io.BytesIO(body))
    assert img.size == (320, 240) and img.mode == "P"
    assert "Retry-After" not in headers

    status, headers2, body = get(f"{base}/frame.png?width=320&height=240&palette=bw",
                                 {"If-None-Match": headers["ETag"]})
    assert status == 304 and body == b""
    assert headers2["ETag"] == headers["ETag"]


def test_defaults_to_1600x1200_spectra6(picture, serve):
    status, _, body = get(f"{serve(FileSource(picture))}/frame.png?mode=collage")
    assert status == 200
    assert Image.open(io.BytesIO(body)).size == (1600, 1200)


def test_changed_file_changes_etag(picture, serve):
    base = serve(FileSource(picture))
    first = get(f"{base}/frame.png?width=40&height=30")[1]["ETag"]
    picture.write_bytes(png_bytes((0, 0, 255)))
    assert get(f"{base}/frame.png?width=40&height=30")[1]["ETag"] != first


@pytest.mark.parametrize("query", ["width=0", "width=abc", "height=99999", "palette=nope"])
def test_bad_parameters_are_rejected(picture, serve, query):
    assert get(f"{serve(FileSource(picture))}/frame.png?{query}")[0] == 400


def test_token_is_required_when_configured(picture, serve):
    base = serve(FileSource(picture), token="s3cret")
    assert get(f"{base}/frame.png?width=40&height=30")[0] == 401
    assert get(f"{base}/frame.png?width=40&height=30", {"Authorization": "Bearer wrong"})[0] == 401
    assert get(f"{base}/frame.png?width=40&height=30", {"Authorization": "Bearer s3cret"})[0] == 200


def test_fixed_retry_after(picture, serve):
    headers = get(f"{serve(FileSource(picture), retry_after=900)}/frame.png?width=40&height=30")[1]
    assert headers["Retry-After"] == "900"


def test_quiet_hours_send_frames_to_sleep_until_morning(picture, serve):
    night = datetime(2026, 9, 29, 23, 0).astimezone()
    base = serve(FileSource(picture), retry_after=900, quiet_hours=QuietHours.parse("22-07"), clock=lambda: night)
    assert get(f"{base}/frame.png?width=40&height=30")[1]["Retry-After"] == str(8 * 3600)


@pytest.mark.parametrize(
    ("window", "now", "left"),
    [
        ("22-07", time(23, 0), 8 * 3600),
        ("22-07", time(6, 30), 30 * 60),
        ("22-07", time(7, 0), 0),
        ("22-07", time(12, 0), 0),
        ("13:30-14:00", time(13, 45), 15 * 60),
        ("13:30-14:00", time(14, 0), 0),
    ],
)
def test_quiet_hours_window(window, now, left):
    assert QuietHours.parse(window).seconds_left(datetime.combine(datetime(2026, 9, 29), now)) == left


@pytest.mark.parametrize("text", ["22", "22-22", "25-07", "a-b"])
def test_quiet_hours_parse_errors(text):
    with pytest.raises(ValueError):
        QuietHours.parse(text)


class FakeSource:
    """A stand-in for fugleramme's /collage.png: ETag, 304, and can go down."""

    def __init__(self):
        self.body = png_bytes((255, 255, 0), (160, 120))
        self.down = False
        self.requests: list[dict[str, str]] = []
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), self._handler())
        self.url = start(self.server) + "/collage.png"

    def _handler(self):
        fake = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                fake.requests.append(dict(self.headers))
                if fake.down:
                    self.send_response(502)
                    self.end_headers()
                    return
                etag = f'"{hash(fake.body)}"'
                if self.headers.get("If-None-Match") == etag:
                    self.send_response(304)
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header("ETag", etag)
                self.send_header("Content-Length", str(len(fake.body)))
                self.end_headers()
                self.wfile.write(fake.body)

            def log_message(self, *args):
                pass

        return Handler

    def close(self):
        self.server.shutdown()
        self.server.server_close()


@pytest.fixture
def fake_source():
    source = FakeSource()
    yield source
    source.close()


def test_proxy_converts_source_and_revalidates_it(fake_source, serve):
    base = serve(UrlSource(fake_source.url, token="src-token"))
    status, headers, body = get(f"{base}/frame.png?width=320&height=240")
    assert status == 200 and Image.open(io.BytesIO(body)).size == (320, 240)
    assert fake_source.requests[0]["Authorization"] == "Bearer src-token"

    # Unchanged source: the proxy sends the source's ETag and gets a 304.
    status, headers2, _ = get(f"{base}/frame.png?width=320&height=240", {"If-None-Match": headers["ETag"]})
    assert status == 304
    assert fake_source.requests[1]["If-None-Match"]

    # Changed source: new frame.
    fake_source.body = png_bytes((0, 0, 255), (160, 120))
    assert get(f"{base}/frame.png?width=320&height=240")[1]["ETag"] != headers["ETag"]


def test_proxy_serves_last_frame_when_source_is_down(fake_source, serve):
    base = serve(UrlSource(fake_source.url))
    etag = get(f"{base}/frame.png?width=40&height=30")[1]["ETag"]
    fake_source.down = True
    status, headers, _ = get(f"{base}/frame.png?width=40&height=30")
    assert status == 200
    assert headers["ETag"] == etag
    assert headers["Retry-After"] == str(UNAVAILABLE_RETRY_S)


def test_proxy_503_when_source_never_answered(fake_source, serve):
    fake_source.down = True
    status, headers, _ = get(f"{serve(UrlSource(fake_source.url))}/frame.png")
    assert status == 503
    assert headers["Retry-After"] == str(UNAVAILABLE_RETRY_S)


def test_firmware_directory_and_manifest(tmp_path, picture, serve):
    firmware = tmp_path / "fw"
    firmware.mkdir()
    binary = firmware / "inkfetch-seeed-ee02.bin"
    binary.write_bytes(b"\x00firmware")
    doc = manifest.build(binary, "seeed-ee02", "v1.2.0", "http://example/inkfetch-seeed-ee02.bin")
    (firmware / "seeed-ee02.json").write_text(manifest.dumps(doc))
    assert doc["version"] == "1.2.0" and len(doc["sha256"]) == 64

    base = serve(FileSource(picture), firmware_dir=firmware)
    status, headers, body = get(f"{base}/firmware/seeed-ee02.json")
    assert status == 200 and json.loads(body) == doc
    assert get(f"{base}/firmware/inkfetch-seeed-ee02.bin")[2] == b"\x00firmware"
    assert get(f"{base}/firmware/../picture.png")[0] == 404
    assert get(f"{base}/firmware/missing.bin")[0] == 404


def test_manifest_rejects_bad_versions(tmp_path):
    binary = tmp_path / "fw.bin"
    binary.write_bytes(b"x")
    with pytest.raises(ValueError):
        manifest.build(binary, "seeed-ee02", "1.2", "http://example/fw.bin")
