"""Named panel palettes.

Each palette has two sets of colours, in the same order:

- ``native``: the reference colour of each ink, as the firmware knows it. The frame
  maps every entry of a PNG palette to the nearest native colour, so this list is
  the contract between server and firmware (see ``nearest_native``).
- ``dither``: the colours the server dithers against and writes into the PNG. They
  can be calibrated to what the glass actually shows, as long as each one stays
  nearest to its own native colour.
"""

from __future__ import annotations

from dataclasses import dataclass

Rgb = tuple[int, int, int]


@dataclass(frozen=True)
class Palette:
    name: str
    native: tuple[Rgb, ...]
    dither: tuple[Rgb, ...]

    def __post_init__(self) -> None:
        if len(self.native) != len(self.dither):
            raise ValueError(f"{self.name}: native and dither palettes differ in length")
        for i, colour in enumerate(self.dither):
            if nearest_native(colour, self.native) != i:
                raise ValueError(f"{self.name}: dither colour {colour} is not nearest to native {i}")


def nearest_native(colour: Rgb, native: tuple[Rgb, ...]) -> int:
    """Index of the nearest native colour: squared RGB distance, lowest index on ties.

    Must stay identical to ``nearestNative`` in the firmware (lib/inkfetch_core).
    """
    best, best_distance = 0, None
    for i, (r, g, b) in enumerate(native):
        distance = (colour[0] - r) ** 2 + (colour[1] - g) ** 2 + (colour[2] - b) ** 2
        if best_distance is None or distance < best_distance:
            best, best_distance = i, distance
    return best


# E Ink Spectra 6. Native order: black, white, yellow, red, blue, green.
_SPECTRA6_PURE: tuple[Rgb, ...] = (
    (0, 0, 0),
    (255, 255, 255),
    (255, 255, 0),
    (255, 0, 0),
    (0, 0, 255),
    (0, 255, 0),
)

# Measured panel colours and blend factor from fugleramme's render/dither.py
# (https://github.com/arnegiacomo/fugleramme, MIT,
# Copyright (c) 2026 Arne Giacomo Munthe-Kaas). Dithering
# against a blend of the pure and the measured colours measures colour distance in
# roughly panel space, which spreads ink more naturally than the pure colours alone.
_SPECTRA6_MEASURED: tuple[Rgb, ...] = (
    (0, 0, 0),
    (161, 164, 165),
    (208, 190, 71),
    (156, 72, 75),
    (61, 59, 94),
    (58, 91, 70),
)
_SPECTRA6_SATURATION = 0.5

SPECTRA6 = Palette(
    name="spectra6",
    native=_SPECTRA6_PURE,
    dither=tuple(
        tuple(int(m * _SPECTRA6_SATURATION + p * (1.0 - _SPECTRA6_SATURATION)) for m, p in zip(meas, pure, strict=True))  # type: ignore[misc]
        for meas, pure in zip(_SPECTRA6_MEASURED, _SPECTRA6_PURE, strict=True)
    ),
)

BW = Palette(name="bw", native=((0, 0, 0), (255, 255, 255)), dither=((0, 0, 0), (255, 255, 255)))

PALETTES: dict[str, Palette] = {p.name: p for p in (SPECTRA6, BW)}
DEFAULT_PALETTE = SPECTRA6.name
