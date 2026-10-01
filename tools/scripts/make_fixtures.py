"""Generate the PNG fixtures shared by the Python tests and the firmware native tests.

Each ``<name>.png`` comes with ``<name>.native``: one byte per pixel, the index of the
nearest Spectra 6 native colour, as the firmware must decode it.

    python tools/scripts/make_fixtures.py        # writes into fixtures/
"""

from __future__ import annotations

import io
import random
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from inkfetch.palettes import SPECTRA6, Rgb, nearest_native  # noqa: E402

FIXTURES = Path(__file__).resolve().parents[2] / "fixtures"
WIDTH, HEIGHT = 64, 48


def _indexed(palette: list[Rgb], bits: int, seed: int) -> tuple[bytes, bytes]:
    rng = random.Random(seed)
    img = Image.new("P", (WIDTH, HEIGHT))
    img.putpalette([c for colour in palette for c in colour])
    pixels = [rng.randrange(len(palette)) for _ in range(WIDTH * HEIGHT)]
    img.putdata(pixels)
    buffer = io.BytesIO()
    img.save(buffer, format="PNG", bits=bits)
    native = bytes(nearest_native(palette[p], SPECTRA6.native) for p in pixels)
    return buffer.getvalue(), native


def fixtures() -> dict[str, bytes]:
    rng = random.Random(8)
    many = [(rng.randrange(256), rng.randrange(256), rng.randrange(256)) for _ in range(200)]
    out: dict[str, bytes] = {}
    for name, palette, bits in (
        ("spectra6-calibrated-4bit", list(SPECTRA6.dither), 4),
        ("bw-1bit", [(0, 0, 0), (255, 255, 255)], 1),
        ("four-2bit", [(10, 10, 10), (250, 250, 250), (240, 20, 20), (20, 20, 240)], 2),
        ("many-8bit", many, 8),
    ):
        png, native = _indexed(palette, bits, seed=len(out))
        out[f"{name}.png"] = png
        out[f"{name}.native"] = native

    # Rejected by the firmware.
    valid = out["spectra6-calibrated-4bit.png"]
    out["truncated.png"] = valid[: len(valid) // 2]
    buffer = io.BytesIO()
    Image.new("RGB", (WIDTH, HEIGHT), (255, 0, 0)).save(buffer, format="PNG")
    out["truecolor.png"] = buffer.getvalue()
    return out


def main() -> None:
    FIXTURES.mkdir(exist_ok=True)
    for name, data in fixtures().items():
        (FIXTURES / name).write_bytes(data)
    print(f"wrote {len(fixtures())} files to {FIXTURES}")


if __name__ == "__main__":
    main()
