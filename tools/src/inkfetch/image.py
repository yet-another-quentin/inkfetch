"""Turn any picture into what an inkfetch frame displays as-is.

Output: an indexed ("P" mode) PNG at the panel's size, already dithered against the
panel palette. The frame does no image processing at all.
"""

from __future__ import annotations

import io
from enum import Enum

from PIL import Image, ImageOps

from .palettes import Palette

DEFAULT_SIZE = (1600, 1200)


class Fit(str, Enum):
    COVER = "cover"  # fill the panel, crop what overflows
    CONTAIN = "contain"  # whole picture, white bars


def fit_to_panel(img: Image.Image, size: tuple[int, int], rotate: int = 0, fit: Fit = Fit.COVER) -> Image.Image:
    """Resize ``img`` for a panel of ``size`` (width, height, as the frame reports it).

    ``rotate`` (0/90/180/270, clockwise) is how the frame hangs: with 90 or 270 the
    picture is laid out in portrait, then turned to the panel's orientation.
    """
    if rotate not in (0, 90, 180, 270):
        raise ValueError("rotate must be 0, 90, 180 or 270")
    width, height = size
    layout = (height, width) if rotate in (90, 270) else (width, height)

    img = ImageOps.exif_transpose(img).convert("RGB")
    if fit is Fit.COVER:
        img = ImageOps.fit(img, layout, Image.Resampling.LANCZOS)
    else:
        img = ImageOps.pad(img, layout, Image.Resampling.LANCZOS, color=(255, 255, 255))
    # Image.rotate turns counter-clockwise.
    return img.rotate(-rotate, expand=True) if rotate else img


def _palette_image(palette: Palette) -> Image.Image:
    flat = [channel for colour in palette.dither for channel in colour]
    # Pad with the first colour so no extra colour can ever be picked.
    flat += list(palette.dither[0]) * (256 - len(palette.dither))
    pal = Image.new("P", (1, 1))
    pal.putpalette(flat)
    return pal


def quantize(img: Image.Image, palette: Palette, dither: bool = True) -> Image.Image:
    """Reduce an RGB image to ``palette``; returns a "P" image with exactly its colours."""
    method = Image.Dither.FLOYDSTEINBERG if dither else Image.Dither.NONE
    quantized = img.convert("RGB").quantize(palette=_palette_image(palette), dither=method)
    # Trim the padded palette so the PNG carries only the panel's colours.
    quantized.putpalette([c for colour in palette.dither for c in colour])
    return quantized


def encode_png(quantized: Image.Image) -> bytes:
    """Smallest-bit-depth indexed PNG. Pillow picks 1/2/4/8 bits from the palette size."""
    colours = len(quantized.getpalette() or []) // 3
    bits = next(b for b in (1, 2, 4, 8) if colours <= 1 << b)
    buffer = io.BytesIO()
    quantized.save(buffer, format="PNG", optimize=True, bits=bits)
    return buffer.getvalue()


def render(
    img: Image.Image,
    palette: Palette,
    size: tuple[int, int] = DEFAULT_SIZE,
    rotate: int = 0,
    fit: Fit = Fit.COVER,
    dither: bool = True,
) -> bytes:
    """Full pipeline: any picture in, frame-ready PNG out."""
    return encode_png(quantize(fit_to_panel(img, size, rotate, fit), palette, dither))
