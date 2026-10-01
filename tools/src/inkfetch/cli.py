"""inkfetch: prepare and serve pictures for inkfetch e-paper frames.

  inkfetch convert photo.jpg frame.png
  inkfetch serve photo.jpg --port 8080
  inkfetch serve --source http://fugleramme.local:8080/collage.png
  inkfetch manifest firmware.bin --env seeed-ee02 --version 1.2.0 --url https://...
"""

from __future__ import annotations

import argparse
import logging
import sys
from pathlib import Path

from PIL import Image

from . import manifest
from .image import DEFAULT_SIZE, Fit, render
from .palettes import DEFAULT_PALETTE, PALETTES
from .server import FileSource, QuietHours, ServerOptions, UrlSource, make_server


def _layout_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--rotate", type=int, default=0, choices=(0, 90, 180, 270),
                        help="how the frame hangs, clockwise (90/270 = portrait)")
    parser.add_argument("--fit", type=Fit, default=Fit.COVER, choices=list(Fit),
                        help="cover: fill and crop; contain: whole picture with white bars")
    parser.add_argument("--no-dither", action="store_true", help="nearest colour, no dithering")


def _convert(args: argparse.Namespace) -> None:
    png = render(Image.open(args.image), PALETTES[args.palette], (args.width, args.height),
                 args.rotate, args.fit, not args.no_dither)
    args.output.write_bytes(png)


def _serve(args: argparse.Namespace) -> None:
    if bool(args.image) == bool(args.source):
        raise SystemExit("serve needs either a picture or --source URL")
    source = UrlSource(args.source, args.source_token) if args.source else FileSource(args.image)
    options = ServerOptions(
        rotate=args.rotate,
        fit=args.fit,
        dither=not args.no_dither,
        token=args.token,
        retry_after=args.retry_after,
        quiet_hours=QuietHours.parse(args.quiet_hours) if args.quiet_hours else None,
        firmware_dir=args.firmware_dir,
    )
    httpd = make_server(source, options, args.bind, args.port)
    logging.info("serving http://%s:%d/frame.png", args.bind, args.port)
    httpd.serve_forever()


def _manifest(args: argparse.Namespace) -> None:
    print(manifest.dumps(manifest.build(args.firmware, args.env, args.version, args.url)), end="")


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(prog="inkfetch", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    convert = sub.add_parser("convert", help="convert a picture into a frame-ready PNG")
    convert.add_argument("image", type=Path)
    convert.add_argument("output", type=Path)
    convert.add_argument("--width", type=int, default=DEFAULT_SIZE[0])
    convert.add_argument("--height", type=int, default=DEFAULT_SIZE[1])
    convert.add_argument("--palette", default=DEFAULT_PALETTE, choices=list(PALETTES))
    _layout_options(convert)
    convert.set_defaults(run=_convert)

    serve = sub.add_parser("serve", help="serve /frame.png to frames")
    serve.add_argument("image", type=Path, nargs="?", help="local picture to serve")
    serve.add_argument("--source", metavar="URL", help="proxy mode: full-colour picture to fetch")
    serve.add_argument("--source-token", help="bearer token sent to --source")
    serve.add_argument("--bind", default="0.0.0.0")
    serve.add_argument("--port", type=int, default=8080)
    serve.add_argument("--token", help="require this bearer token from frames")
    serve.add_argument("--retry-after", type=int, metavar="SECONDS",
                       help="ask frames to come back after this many seconds")
    serve.add_argument("--quiet-hours", metavar="HH-HH",
                       help="daily window (server local time) during which frames sleep, e.g. 22-07")
    serve.add_argument("--firmware-dir", type=Path, help="serve OTA manifests and binaries under /firmware/")
    _layout_options(serve)
    serve.set_defaults(run=_serve)

    man = sub.add_parser("manifest", help="print the OTA manifest for a firmware binary")
    man.add_argument("firmware", type=Path)
    man.add_argument("--env", required=True, help="PlatformIO environment, e.g. seeed-ee02")
    man.add_argument("--version", required=True)
    man.add_argument("--url", required=True, help="where frames download the binary")
    man.set_defaults(run=_manifest)

    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    args.run(args)


if __name__ == "__main__":
    sys.exit(main())
