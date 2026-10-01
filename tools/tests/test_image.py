import io
import sys
from pathlib import Path

import pytest
from PIL import Image

from inkfetch.image import Fit, fit_to_panel, render
from inkfetch.palettes import BW, PALETTES, SPECTRA6, Palette, nearest_native

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import make_fixtures  # noqa: E402


def decode(png: bytes) -> Image.Image:
    img = Image.open(io.BytesIO(png))
    img.load()
    return img


def test_calibrated_spectra6_stays_nearest_to_pure_colours():
    for i, colour in enumerate(SPECTRA6.dither):
        assert nearest_native(colour, SPECTRA6.native) == i


def test_palette_rejects_dither_colour_nearer_to_another_ink():
    with pytest.raises(ValueError):
        Palette("broken", native=((0, 0, 0), (255, 255, 255)), dither=((200, 200, 200), (255, 255, 255)))


def test_nearest_native_prefers_lowest_index_on_ties():
    assert nearest_native((128, 128, 128), ((0, 0, 0), (256, 256, 256))) == 0


@pytest.mark.parametrize("palette", PALETTES.values(), ids=list(PALETTES))
def test_render_is_indexed_png_with_panel_size_and_palette(palette):
    img = decode(render(Image.new("RGB", (300, 200), (200, 50, 50)), palette, (1600, 1200)))
    assert img.format == "PNG" and img.mode == "P"
    assert img.size == (1600, 1200)
    colours = img.getpalette()[: 3 * len(palette.dither)]
    assert [tuple(colours[i : i + 3]) for i in range(0, len(colours), 3)] == list(palette.dither)
    assert max(img.tobytes()) < len(palette.dither)


def test_solid_native_colours_come_out_unchanged():
    for i, colour in enumerate(SPECTRA6.native):
        img = decode(render(Image.new("RGB", (16, 12), colour), SPECTRA6, (16, 12)))
        assert set(img.tobytes()) == {i}


def test_png_uses_smallest_bit_depth():
    spectra = render(Image.new("RGB", (8, 8)), SPECTRA6, (8, 8))
    bw = render(Image.new("RGB", (8, 8)), BW, (8, 8))
    assert spectra[24] == 4  # IHDR bit depth
    assert bw[24] == 1


def test_portrait_frame_is_laid_out_then_turned():
    img = Image.new("RGB", (600, 800))
    assert fit_to_panel(img, (1600, 1200), rotate=90).size == (1600, 1200)
    assert fit_to_panel(img, (1600, 1200), fit=Fit.CONTAIN).size == (1600, 1200)
    with pytest.raises(ValueError):
        fit_to_panel(img, (1600, 1200), rotate=45)


def test_fixtures_match_the_generator():
    """The committed fixtures decode to what the generator produces today."""
    root = Path(make_fixtures.FIXTURES)
    for name, data in make_fixtures.fixtures().items():
        committed = (root / name).read_bytes()
        if name.endswith(".native"):
            assert committed == data, name
        elif name == "truncated.png":
            with pytest.raises(OSError):
                decode(committed)
        else:
            assert decode(committed).tobytes() == decode(data).tobytes(), name
