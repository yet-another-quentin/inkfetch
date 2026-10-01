# inkfetch

An e-paper frame that **pulls** its picture. The frame wakes up, fetches a PNG over
HTTP(S), redraws the panel only if the picture changed, and goes back to deep sleep.
All image work (layout, resizing, dithering) happens on the server, so the firmware
stays small and any server that speaks the [contract](#frame--server-contract) can
feed it.

- **Firmware** (`firmware/`): ESP32, PlatformIO + Arduino. Display-specific code
  lives in one *profile* file per display.
- **Tools** (`tools/`): `inkfetch`, a Python CLI to convert pictures and to run a
  reference server, including a proxy mode for
  [fugleramme](https://github.com/arnegiacomo/fugleramme).

Supported displays:

| Profile | Hardware | Panel |
|---|---|---|
| `seeed-ee02` | [Seeed XIAO ePaper Display Board EE02](https://wiki.seeedstudio.com/getting_started_with_ee02/) (XIAO ESP32-S3 Plus) | 13.3" E Ink Spectra 6, 1600×1200, 6 colours |

> [!NOTE]
> The firmware has not yet been tested on hardware. Everything that does not need
> the panel is covered by host tests (see [Development](#development)).

## How it works

```
server ── GET /frame.png?width=1600&height=1200&palette=spectra6 ──▶ 200 PNG + ETag
   ▲                                                                 304 unchanged
   │                                                                 Retry-After: N
frame: wake (timer / button) → Wi-Fi → GET → redraw if changed → deep sleep
```

- **Change detection**: the frame sends `If-None-Match` with the ETag of the picture
  on screen; servers without ETags still work (the frame compares a CRC32).
- **Panel protection**, from the panel vendor's guidance, whatever triggered the
  wake-up:
  - at least **180 s** between two refreshes;
  - at least one refresh every **24 h**, redrawing the same picture if needed;
  - the panel is put to deep sleep and unpowered before the ESP32 sleeps.
- **Sleep length**: the configured poll interval, or the server's `Retry-After`
  (clamped to 180 s–24 h), never past the next maintenance refresh. If that refresh
  fails (no Wi-Fi, server down), the frame retries at the normal interval.
- **Resets**: the refresh history survives software, crash and brownout resets, so
  the limits hold across them. After a brownout (battery too low) the frame sleeps
  for 24 h; after three crashes in a row, for an hour.

### Buttons (`seeed-ee02`)

| Button | Short press | Other |
|---|---|---|
| 1 (GPIO2) | refresh now | held 3 s: blank the panel for storage, sleep until a button press |
| 2 (GPIO3) | next mode from the configured list (sent as `mode=`) | |
| 3 (GPIO5) | open the configuration portal | also when held at power-on |

## Frame ↔ server contract

**Request**: `GET <image URL>` with these query parameters added (servers may ignore
them):

| Parameter | Example | Meaning |
|---|---|---|
| `width`, `height` | `1600`, `1200` | panel size, as the picture must be sent |
| `palette` | `spectra6` | the panel's inks (see below) |
| `mode` | `latest` | current entry of the frame's mode list, if one is configured |
| `id` | `a1b2c3d4e5f6` | the frame's MAC address, to tell frames apart |
| `fw` | `0.1.0` | firmware version |

Headers: `If-None-Match` (unless a refresh is forced) and, if a token is configured,
`Authorization: Bearer <token>`. Redirects are followed, except when a token is
configured (so it is never sent to another host).

**Response**:

- `200` with an **indexed PNG** (palette colour type, 1/2/4/8 bits, not interlaced,
  at most 2 MB), exactly `width`×`height`, already dithered and oriented. The frame maps each
  palette entry to the nearest native ink (squared RGB distance) and does no other
  processing; anything else is rejected.
- `304` when `If-None-Match` matches: nothing to do.
- Optional `ETag`, `Cache-Control: no-cache`, and `Retry-After: <seconds>` (the
  HTTP-date form is not supported: the frame does not know the time).

**Palettes**: `native` colours are the contract; the PNG may carry calibrated
colours, as long as each stays nearest to its native ink.

| Name | Native inks, in order |
|---|---|
| `spectra6` | black `#000000`, white `#FFFFFF`, yellow `#FFFF00`, red `#FF0000`, blue `#0000FF`, green `#00FF00` |
| `bw` | black, white |

The reference implementation is `tools/src/inkfetch/palettes.py`; its `spectra6`
dithering colours are the calibrated ones from fugleramme's `render/dither.py`
(MIT, © 2026 Arne Giacomo Munthe-Kaas).

### Quiet hours

The frame has no clock. To let it sleep through the night, the server answers with
`Retry-After: <seconds until morning>`. Button presses still wake the frame, and the
24 h maintenance refresh still applies. `inkfetch serve --quiet-hours 22-07` does
this.

## Getting started

### 1. Run a server

```sh
cd tools
python3 -m venv .venv && . .venv/bin/activate
pip install -e '.[dev]'

inkfetch serve photo.jpg                     # http://<host>:8080/frame.png
```

Useful options: `--rotate 90` (frame hung in portrait), `--fit contain`,
`--token SECRET`, `--retry-after 3600`, `--quiet-hours 22-07`,
`--firmware-dir DIR` (serves OTA files under `/firmware/`).
`inkfetch convert photo.jpg frame.png` writes the PNG the frame would receive.

### 2. Flash the firmware

With [PlatformIO](https://platformio.org/install/cli):

```sh
cd firmware
pio run -e seeed-ee02 -t upload
pio device monitor
```

Logs go over USB CDC (on the EE02, GPIO43 powers the panel).

### 3. Configure the frame

On first boot (or with button 3), the frame opens a Wi-Fi access point
`inkfetch-XXXX` and shows its password and a QR code on the panel. Join it, open
`http://192.168.4.1`, fill **Setup** first, then **Configure WiFi**. The portal
closes once the frame is configured and online (a minute after saving the settings,
to leave time for the Wi-Fi page), or after 10 minutes; the frame then restarts and
starts fetching.

| Setting | Notes |
|---|---|
| Image URL | `http://` or `https://` (public CAs) |
| Bearer token | stored, never shown again; leave empty to keep it, or tick *Clear* |
| Modes | e.g. `collage,latest,arrival`; button 2 cycles through them |
| Poll interval | 60 s–24 h, default 15 min |
| Follow Retry-After | on by default |
| OTA manifest URL | empty disables updates |
| OTA check | every 1–720 h, default 24 h |
| Protection limits | can only be made *more* cautious than the panel's |
| Hostname | default `inkfetch` |

For development, `include/secrets.example.h` → `include/secrets.h` pre-fills the
image URL and Wi-Fi credentials.

## Using with fugleramme

inkfetch works with an unmodified [fugleramme](https://github.com/arnegiacomo/fugleramme):
its `/collage.png` is full colour, so `inkfetch serve` fetches it, revalidates it with
its ETag, and serves a dithered PNG at the frame's size.

```sh
inkfetch serve --source http://fugleramme.local:8080/collage.png --quiet-hours 22-07
```

In fugleramme's admin page, turn **Lock to panel** off and set **Aspect** to 4:3 and
**Resolution** to 1440p, so the collage has the panel's shape and is reduced rather
than enlarged. If fugleramme is unreachable, the last
picture keeps being served with a short `Retry-After`.

Limits of the proxy: the mode is the one chosen in fugleramme's admin (button 2 only
forces a refresh), and dithering is done by inkfetch rather than fugleramme.
[`examples/docker-compose.fugleramme.yml`](examples/docker-compose.fugleramme.yml)
runs the proxy next to fugleramme.

## OTA updates

The frame checks a manifest (every 24 h by default, never on a button press):

```json
{"env": "seeed-ee02", "version": "1.2.0",
 "url": "https://…/inkfetch-seeed-ee02.bin", "sha256": "…"}
```

It installs the binary when `env` matches its build and `version` is newer, checks
the SHA-256, and restarts. The new firmware is confirmed only once it reaches the
image server; otherwise the bootloader rolls back to the previous one, and that
version is never installed again (a newer one will be).
`inkfetch manifest firmware.bin --env seeed-ee02 --version 1.2.0 --url …` writes a
manifest. Pushing a `vX.Y.Z` tag makes CI publish a GitHub release with one binary
and one manifest per profile, so frames can point at
`https://github.com/yet-another-quentin/inkfetch/releases/latest/download/seeed-ee02.json`.

## Adding a display

1. Create `firmware/src/display/<profile>.cpp` implementing `inkfetch::Display` and
   `inkfetch::buttons()` from [`firmware/src/profile.h`](firmware/src/profile.h):
   panel size, native palette, protection limits, row writing, refresh, power-off.
   Buttons must be active-low on RTC-capable GPIOs, so they can wake the board.
   [`seeed_ee02.cpp`](firmware/src/display/seeed_ee02.cpp) is the example.
2. Add an `[env:<profile>]` to `firmware/platformio.ini` extending `esp32`, with its
   board, libraries, and `build_src_filter = +<*> -<display/> +<display/<profile>.cpp>`.
3. If the panel has a new set of inks, add its palette to
   `tools/src/inkfetch/palettes.py` (and the table above).
4. Add the profile to the CI matrix and to the table at the top.

## Development

```sh
(cd firmware && pio test -e native)        # firmware logic, no hardware
(cd firmware && pio run -e seeed-ee02)     # firmware build
(cd tools && pytest)                       # converter, server, proxy, manifest
python tools/scripts/make_fixtures.py      # regenerate fixtures/ after a palette change
```

`firmware/lib/inkfetch_core` holds the hardware-independent logic (PNG decoding,
palette mapping, refresh policy, Retry-After, OTA manifest, settings validation). It
builds on the host, and shares its PNG fixtures (`fixtures/`, generated by
`tools/scripts/make_fixtures.py`) with the Python tests.

## Roadmap

- [ ] Test on hardware: refresh time, deep-sleep current, panel power pin (GPIO43)
      cutting the whole panel (it is also UART0 TX, so it idles high during boot
      until the firmware drives it low), deep sleep of the second controller.
- [ ] Keep the last picture in flash, so the maintenance refresh works offline.
- [ ] Battery level (EE02 ADC pin to find) sent to the server.
- [ ] Error page on the panel after repeated failures.
- [ ] MQTT option (notification of a new picture).
- [ ] fugleramme: a public `/frame.png?mode=…` route (PR upstream), so the frame
      can pick the mode and fugleramme does the dithering.
- [ ] Calibrate the Spectra 6 palette on the EE02 panel.

## License

[MIT](LICENSE).
