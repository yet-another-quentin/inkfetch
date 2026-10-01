"""Reference inkfetch server.

Serves ``/frame.png`` following the frame contract (see the README):

- honours ``width``, ``height`` and ``palette`` query parameters sent by the frame;
- ETag + ``Cache-Control: no-cache``, 304 on ``If-None-Match``;
- optional bearer token, fixed ``Retry-After`` and quiet hours;
- optional static ``/firmware/`` directory for OTA manifests and binaries.

The picture comes either from a local file or, in proxy mode, from any URL serving
a full-colour image (such as fugleramme's ``/collage.png``).
"""

from __future__ import annotations

import hashlib
import hmac
import io
import logging
import threading
import urllib.error
import urllib.request
from collections import OrderedDict
from collections.abc import Callable
from dataclasses import dataclass, field
from datetime import datetime, time, timedelta
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from PIL import Image

from .image import DEFAULT_SIZE, Fit, render
from .palettes import DEFAULT_PALETTE, PALETTES

log = logging.getLogger(__name__)

MAX_SIDE = 4096
# Retry-After sent when the source is down: soon, but the frame clamps it anyway.
UNAVAILABLE_RETRY_S = 300


class SourceUnavailable(Exception):
    pass


@dataclass(frozen=True)
class SourceImage:
    key: str  # changes whenever the picture changes
    image: Image.Image
    stale: bool = False  # the source is down; this is the last good copy


class FileSource:
    """A local picture, re-read when the file changes."""

    def __init__(self, path: Path):
        self.path = path
        self._lock = threading.Lock()
        self._cached: SourceImage | None = None

    def fetch(self) -> SourceImage:
        try:
            stat = self.path.stat()
        except OSError as exc:
            raise SourceUnavailable(str(exc)) from exc
        key = f"{stat.st_mtime_ns}-{stat.st_size}"
        with self._lock:
            if self._cached is None or self._cached.key != key:
                with Image.open(self.path) as img:
                    img.load()
                    self._cached = SourceImage(key, img.copy())
            return self._cached


class UrlSource:
    """A full-colour picture fetched over HTTP, revalidated with its ETag."""

    def __init__(self, url: str, token: str | None = None, timeout: float = 30):
        self.url = url
        self.token = token
        self.timeout = timeout
        self._lock = threading.Lock()
        self._etag: str | None = None
        self._cached: SourceImage | None = None

    def fetch(self) -> SourceImage:
        with self._lock:
            headers = {}
            if self._etag and self._cached:
                headers["If-None-Match"] = self._etag
            if self.token:
                headers["Authorization"] = f"Bearer {self.token}"
            request = urllib.request.Request(self.url, headers=headers)
            try:
                with urllib.request.urlopen(request, timeout=self.timeout) as resp:
                    body = resp.read()
                    etag = resp.headers.get("ETag")
            except urllib.error.HTTPError as exc:
                if exc.code == HTTPStatus.NOT_MODIFIED and self._cached:
                    return self._cached
                return self._fallback(f"{self.url}: HTTP {exc.code}")
            except (urllib.error.URLError, OSError) as exc:
                return self._fallback(f"{self.url}: {exc}")

            try:
                with Image.open(io.BytesIO(body)) as img:
                    img.load()
                    image = img.copy()
            except OSError as exc:
                return self._fallback(f"{self.url}: not an image ({exc})")
            self._etag = etag
            self._cached = SourceImage(hashlib.sha256(body).hexdigest()[:32], image)
            return self._cached

    def _fallback(self, reason: str) -> SourceImage:
        log.warning("source unavailable: %s", reason)
        if self._cached is None:
            raise SourceUnavailable(reason)
        return SourceImage(self._cached.key, self._cached.image, stale=True)


@dataclass(frozen=True)
class QuietHours:
    """A daily window, possibly across midnight, in the server's local time."""

    start: time
    end: time

    @classmethod
    def parse(cls, text: str) -> QuietHours:
        """``22-07`` or ``22:30-07:15``."""
        try:
            start, end = (cls._time(part) for part in text.split("-"))
        except ValueError as exc:
            raise ValueError(f"quiet hours must look like 22-07 or 22:30-07:00, got {text!r}") from exc
        if start == end:
            raise ValueError("quiet hours start and end must differ")
        return cls(start, end)

    @staticmethod
    def _time(part: str) -> time:
        hour, _, minute = part.strip().partition(":")
        return time(int(hour), int(minute or 0))

    def seconds_left(self, now: datetime) -> int:
        """Seconds until the window ends, 0 outside it."""
        current = now.time()
        inside = (
            self.start <= current < self.end
            if self.start < self.end
            else current >= self.start or current < self.end
        )
        if not inside:
            return 0
        end = datetime.combine(now.date(), self.end, tzinfo=now.tzinfo)
        if end <= now:
            end += timedelta(days=1)
        return int((end - now).total_seconds())


@dataclass
class ServerOptions:
    rotate: int = 0
    fit: Fit = Fit.COVER
    dither: bool = True
    token: str | None = None
    retry_after: int | None = None
    quiet_hours: QuietHours | None = None
    firmware_dir: Path | None = None
    clock: Callable[[], datetime] = field(default=lambda: datetime.now().astimezone())


class FrameRenderer:
    """Renders frames per (source picture, size, palette), keeping the last few."""

    def __init__(self, options: ServerOptions, capacity: int = 8):
        self.options = options
        self.capacity = capacity
        self._lock = threading.Lock()
        self._cache: OrderedDict[tuple, tuple[bytes, str]] = OrderedDict()

    def frame(self, source: SourceImage, size: tuple[int, int], palette: str) -> tuple[bytes, str]:
        key = (source.key, size, palette)
        with self._lock:
            if key in self._cache:
                self._cache.move_to_end(key)
                return self._cache[key]
        png = render(source.image, PALETTES[palette], size, self.options.rotate, self.options.fit, self.options.dither)
        entry = (png, '"' + hashlib.sha256(png).hexdigest()[:32] + '"')
        with self._lock:
            self._cache[key] = entry
            while len(self._cache) > self.capacity:
                self._cache.popitem(last=False)
        return entry


class BadRequest(Exception):
    pass


def frame_params(query: dict[str, list[str]]) -> tuple[tuple[int, int], str]:
    """Panel size and palette announced by the frame, with the defaults when absent."""

    def side(name: str, default: int) -> int:
        raw = query.get(name, [str(default)])[0]
        if not raw.isdigit() or not 0 < int(raw) <= MAX_SIDE:
            raise BadRequest(f"{name} must be an integer between 1 and {MAX_SIDE}")
        return int(raw)

    size = (side("width", DEFAULT_SIZE[0]), side("height", DEFAULT_SIZE[1]))
    palette = query.get("palette", [DEFAULT_PALETTE])[0]
    if palette not in PALETTES:
        raise BadRequest(f"unknown palette {palette!r}, expected one of {', '.join(PALETTES)}")
    return size, palette


def make_handler(source: FileSource | UrlSource, options: ServerOptions) -> type[BaseHTTPRequestHandler]:
    renderer = FrameRenderer(options)

    class Handler(BaseHTTPRequestHandler):
        server_version = "inkfetch"

        def do_GET(self) -> None:
            url = urlparse(self.path)
            if url.path == "/frame.png":
                self._frame(parse_qs(url.query))
            elif url.path == "/health":
                self._send(HTTPStatus.OK, b"ok\n", "text/plain")
            elif url.path.startswith("/firmware/") and options.firmware_dir:
                self._firmware(url.path.removeprefix("/firmware/"))
            else:
                self._send(HTTPStatus.NOT_FOUND, b"not found\n", "text/plain")

        def _authorized(self) -> bool:
            if not options.token:
                return True
            expected = f"Bearer {options.token}"
            return hmac.compare_digest(self.headers.get("Authorization", ""), expected)

        def _retry_after(self, stale: bool) -> int | None:
            # Quiet hours first: nobody looks at the frame, even if the source is down.
            if options.quiet_hours and (quiet := options.quiet_hours.seconds_left(options.clock())):
                return quiet
            if stale:
                return UNAVAILABLE_RETRY_S
            return options.retry_after

        def _frame(self, query: dict[str, list[str]]) -> None:
            if not self._authorized():
                self._send(HTTPStatus.UNAUTHORIZED, b"missing or wrong token\n", "text/plain",
                           {"WWW-Authenticate": "Bearer"})
                return
            try:
                size, palette = frame_params(query)
            except BadRequest as exc:
                self._send(HTTPStatus.BAD_REQUEST, f"{exc}\n".encode(), "text/plain")
                return
            try:
                picture = source.fetch()
            except SourceUnavailable as exc:
                self._send(HTTPStatus.SERVICE_UNAVAILABLE, f"{exc}\n".encode(), "text/plain",
                           {"Retry-After": str(self._retry_after(stale=True))})
                return

            png, etag = renderer.frame(picture, size, palette)
            headers = {"ETag": etag, "Cache-Control": "no-cache"}
            if (retry := self._retry_after(picture.stale)) is not None:
                headers["Retry-After"] = str(retry)
            if self.headers.get("If-None-Match") == etag:
                self._send(HTTPStatus.NOT_MODIFIED, b"", None, headers)
            else:
                self._send(HTTPStatus.OK, png, "image/png", headers)

        def _firmware(self, name: str) -> None:
            assert options.firmware_dir is not None
            root = options.firmware_dir.resolve()
            path = (root / name).resolve()
            if root not in path.parents or not path.is_file():
                self._send(HTTPStatus.NOT_FOUND, b"not found\n", "text/plain")
                return
            content_type = "application/json" if path.suffix == ".json" else "application/octet-stream"
            self._send(HTTPStatus.OK, path.read_bytes(), content_type)

        def _send(self, status: HTTPStatus, body: bytes, content_type: str | None,
                  headers: dict[str, str] | None = None) -> None:
            self.send_response(status)
            if content_type:
                self.send_header("Content-Type", content_type)
            for name, value in (headers or {}).items():
                self.send_header(name, value)
            if status != HTTPStatus.NOT_MODIFIED:
                self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if body:
                self.wfile.write(body)

        def log_message(self, format: str, *args: object) -> None:
            log.info("%s %s", self.address_string(), format % args)

    return Handler


def make_server(source: FileSource | UrlSource, options: ServerOptions, bind: str, port: int) -> ThreadingHTTPServer:
    return ThreadingHTTPServer((bind, port), make_handler(source, options))
