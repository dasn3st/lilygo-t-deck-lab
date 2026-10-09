#!/usr/bin/env python3
"""Stage a Berlin OSM-Carto PMTiles copy with blue Wi-Fi glyphs from z16.

This never edits the active map pack. It writes a new PMTiles file under outputs/.
Requires Pillow, osmium, and the Python `pmtiles` package.
"""

from __future__ import annotations

import argparse
import io
import math
from collections import defaultdict
from pathlib import Path

import osmium
from PIL import Image, ImageDraw, ImageFilter
from pmtiles.reader import all_tiles
from pmtiles.tile import zxy_to_tileid
from pmtiles.writer import Writer


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASE = ROOT / "data/OSM-Carto.pmtiles"
DEFAULT_PBF = ROOT / "data/berlin-latest.osm.pbf"
DEFAULT_ICON = ROOT / "tools/assets/wifi-symbol-reference.png"
DEFAULT_OUTPUT = ROOT / "outputs/wlan-icon-staging/WLAN-z16-blue-icons.pmtiles"
MIN_LON, MIN_LAT, MAX_LON, MAX_LAT = 13.0879999, 52.338, 13.761, 52.675
BLUE = (25, 132, 255, 255)
HALO = (255, 255, 255, 255)
TILE_SIZE = 256


def useful(tags) -> bool:
    return tags.get("wifi") in {"free", "yes"} or tags.get("internet_access") in {"wlan", "wifi"}


class WifiHandler(osmium.SimpleHandler):
    def __init__(self) -> None:
        super().__init__()
        self.points: dict[tuple[float, float], tuple[float, float]] = {}

    def add(self, lon: float, lat: float) -> None:
        if MIN_LON <= lon <= MAX_LON and MIN_LAT <= lat <= MAX_LAT:
            key = (round(lon, 6), round(lat, 6))
            self.points[key] = (lon, lat)

    def node(self, node) -> None:
        if useful(node.tags) and node.location.valid():
            self.add(node.location.lon, node.location.lat)

    def way(self, way) -> None:
        if not useful(way.tags):
            return
        coords = [(n.lon, n.lat) for n in way.nodes if n.location.valid()]
        if coords:
            self.add(sum(x for x, _ in coords) / len(coords), sum(y for _, y in coords) / len(coords))


def world_pixel(lon: float, lat: float, zoom: int) -> tuple[float, float]:
    n = 1 << zoom
    x = (lon + 180.0) / 360.0 * n * TILE_SIZE
    lat_rad = math.radians(max(-85.05112878, min(85.05112878, lat)))
    y = (0.5 - math.log((1 + math.sin(lat_rad)) / (1 - math.sin(lat_rad))) / (4 * math.pi)) * n * TILE_SIZE
    return x, y


def make_icon(source_path: Path, size: tuple[int, int] = (20, 18)) -> Image.Image:
    source = Image.open(source_path).convert("L")
    # The supplied reference has a black field and a pale Wi-Fi glyph. Threshold the
    # artwork, crop its margins, and retain only the glyph as an alpha mask.
    mask = source.point(lambda value: 255 if value >= 150 else 0)
    bbox = mask.getbbox()
    if not bbox:
        raise ValueError(f"No light Wi-Fi glyph found in {source_path}")
    mask = mask.crop(bbox).resize(size, Image.Resampling.LANCZOS)
    halo_mask = mask.filter(ImageFilter.MaxFilter(3))
    icon = Image.new("RGBA", size, (0, 0, 0, 0))
    # Draw a fine white keyline for legibility on both light and dark map features.
    expanded = Image.new("RGBA", size, (0, 0, 0, 0))
    expanded.putalpha(halo_mask)
    blue = Image.new("RGBA", size, BLUE)
    blue.putalpha(mask)
    icon.alpha_composite(expanded)
    icon.alpha_composite(blue)
    return icon


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, default=DEFAULT_BASE)
    parser.add_argument("--pbf", type=Path, default=DEFAULT_PBF)
    parser.add_argument("--icon", type=Path, default=DEFAULT_ICON)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    for path in (args.base, args.pbf, args.icon):
        if not path.is_file():
            raise SystemExit(f"Required input not found: {path}")
    if args.output.resolve() in {args.base.resolve(), (ROOT / "data/WLAN-active.pmtiles").resolve()}:
        raise SystemExit("Refusing to overwrite an active PMTiles source")

    print("Extracting mapped Berlin Wi-Fi locations from OSM PBF…", flush=True)
    handler = WifiHandler()
    handler.apply_file(str(args.pbf), locations=True)
    points = list(handler.points.values())
    print(f"Found {len(points)} unique Wi-Fi locations in the map bounds.", flush=True)

    icon = make_icon(args.icon)
    marker_buckets: dict[tuple[int, int, int], list[tuple[float, float]]] = defaultdict(list)
    half_w, half_h = icon.width / 2, icon.height / 2
    for zoom in (16, 17):
        for lon, lat in points:
            px, py = world_pixel(lon, lat, zoom)
            tx, ty = int(px // TILE_SIZE), int(py // TILE_SIZE)
            lx, ly = px - tx * TILE_SIZE, py - ty * TILE_SIZE
            marker_buckets[(zoom, tx, ty)].append((lx, ly))
            # Repeat at tile edges so the symbol is not clipped at a tile boundary.
            if lx < half_w:
                marker_buckets[(zoom, tx - 1, ty)].append((lx + TILE_SIZE, ly))
            if lx > TILE_SIZE - half_w:
                marker_buckets[(zoom, tx + 1, ty)].append((lx - TILE_SIZE, ly))
            if ly < half_h:
                marker_buckets[(zoom, tx, ty - 1)].append((lx, ly + TILE_SIZE))
            if ly > TILE_SIZE - half_h:
                marker_buckets[(zoom, tx, ty + 1)].append((lx, ly - TILE_SIZE))
            if lx < half_w and ly < half_h:
                marker_buckets[(zoom, tx - 1, ty - 1)].append((lx + TILE_SIZE, ly + TILE_SIZE))
            if lx < half_w and ly > TILE_SIZE - half_h:
                marker_buckets[(zoom, tx - 1, ty + 1)].append((lx + TILE_SIZE, ly - TILE_SIZE))
            if lx > TILE_SIZE - half_w and ly < half_h:
                marker_buckets[(zoom, tx + 1, ty - 1)].append((lx - TILE_SIZE, ly + TILE_SIZE))
            if lx > TILE_SIZE - half_w and ly > TILE_SIZE - half_h:
                marker_buckets[(zoom, tx + 1, ty + 1)].append((lx - TILE_SIZE, ly - TILE_SIZE))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    partial = args.output.with_suffix(args.output.suffix + ".partial")
    if args.output.exists() or partial.exists():
        raise SystemExit(f"Output already exists; choose a new path: {args.output}")

    print(f"Writing new staged map to {args.output}…", flush=True)
    with args.base.open("rb") as src, partial.open("xb") as dst:
        def get_bytes(offset: int, length: int) -> bytes:
            src.seek(offset)
            return src.read(length)

        tiles = all_tiles(get_bytes)
        writer = Writer(dst)
        updated = 0
        total = 0
        for (zoom, tx, ty), data in tiles:
            marks = marker_buckets.get((zoom, tx, ty))
            if marks:
                image = Image.open(io.BytesIO(data)).convert("RGBA")
                for cx, cy in marks:
                    image.alpha_composite(icon, (round(cx - half_w), round(cy - half_h)))
                out = io.BytesIO()
                image.convert("RGB").quantize(colors=256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE).save(out, format="PNG", optimize=True)
                data = out.getvalue()
                updated += 1
            writer.write_tile(zxy_to_tileid(zoom, tx, ty), data)
            total += 1
            if total % 10000 == 0:
                print(f"  {total} tiles copied; {updated} rendered with WLAN symbols", flush=True)
        from pmtiles.reader import Reader, MmapSource
        reader = Reader(MmapSource(src))
        metadata = reader.metadata()
        metadata["name"] = "Berlin WLAN – blaues Symbol ab z16"
        metadata["description"] = "OSM-Carto mit blauem WLAN-Symbol ab Zoomstufe 16; Daten © OpenStreetMap contributors."
        writer.finalize(dict(reader.header()), metadata)
    partial.replace(args.output)
    print(f"Done: {total} tiles copied, {updated} WLAN tiles rendered.", flush=True)


if __name__ == "__main__":
    main()
