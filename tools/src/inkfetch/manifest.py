"""OTA manifest: the static JSON a frame reads to decide whether to update itself.

    {"env": "seeed-ee02", "version": "1.2.0",
     "url": "https://.../inkfetch-seeed-ee02.bin", "sha256": "<64 hex chars>"}

The frame updates when ``env`` matches its build and ``version`` is newer than its own.
"""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

_VERSION = re.compile(r"^v?\d+\.\d+\.\d+$")


def build(firmware: Path, env: str, version: str, url: str) -> dict[str, str]:
    if not _VERSION.match(version):
        raise ValueError(f"version must look like 1.2.3, got {version!r}")
    return {
        "env": env,
        "version": version.removeprefix("v"),
        "url": url,
        "sha256": hashlib.sha256(firmware.read_bytes()).hexdigest(),
    }


def dumps(manifest: dict[str, str]) -> str:
    return json.dumps(manifest, indent=2) + "\n"
