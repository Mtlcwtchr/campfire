#!/usr/bin/env python3
"""Download reference DEMs; decode Terrarium BEFORE resampling, never screen gray.

Use Python 3.10+ and install tools/terrain_references_requirements.txt.
The authored region manifest stays in content/config; raw tiles and generated
height libraries stay in ignored directories. No runtime generator changes.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import io
import json
import math
import os
from pathlib import Path
import tempfile
import time
import urllib.error
import urllib.request

import numpy as np
from PIL import Image, ImageDraw
from pyproj import CRS, Transformer

ROOT = Path(__file__).resolve().parents[1]
PIPELINE_VERSION = 2
TILE_SIZE = 256
MAX_DOWNLOAD = 16 * 1024 * 1024
USER_AGENT = "ASR-TerrainReferenceImporter/1.0 (public elevation research)"


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, suffix=".tmp", delete=False) as out:
            temporary = Path(out.name)
            out.write(data)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def json_bytes(value) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def image_bytes(image: Image.Image, format: str = "PNG") -> bytes:
    out = io.BytesIO()
    image.save(out, format=format)
    return out.getvalue()


def tile_height(data: bytes) -> np.ndarray:
    with Image.open(io.BytesIO(data), formats=["PNG"]) as image:
        if image.size != (TILE_SIZE, TILE_SIZE) or image.mode not in ("RGB", "RGBA"):
            raise ValueError(f"invalid Terrarium tile: {image.size}, {image.mode}")
        pixels = np.asarray(image, dtype=np.float32)
        result = pixels[:, :, 0] * 256 + pixels[:, :, 1] + pixels[:, :, 2] / 256 - 32768
        if pixels.shape[2] == 4:
            result[pixels[:, :, 3] == 0] = np.nan
        result[result <= -32000] = np.nan
        return result


def fetch(url: str, path: Path, tile: bool = False) -> bytes:
    if path.exists():
        data = path.read_bytes()
        try:
            if tile:
                tile_height(data)
            elif not data:
                raise ValueError("empty document")
            return data
        except (ValueError, OSError):
            path.unlink(missing_ok=True)
    for attempt in range(6):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(request, timeout=40) as response:
                data = response.read(MAX_DOWNLOAD + 1)
            if not data or len(data) > MAX_DOWNLOAD:
                raise ValueError("empty or oversized download")
            if tile:
                tile_height(data)
            atomic_write(path, data)
            return data
        except (urllib.error.URLError, TimeoutError, OSError, ValueError) as error:
            if attempt == 5 or isinstance(error, urllib.error.HTTPError) and error.code == 404:
                raise RuntimeError(f"cannot download {url}: {error}") from error
            time.sleep(min(20, 2 ** attempt))
    raise AssertionError("unreachable")


def source_zoom(latitude: float, spacing: float, maximum: int) -> int:
    # Closest available Web Mercator grid to the requested ground spacing.
    return max(1, min(maximum, round(math.log2(156543.033928 * math.cos(math.radians(latitude)) / spacing))))


def input_key(region: dict, config: dict) -> str:
    inputs = {"pipeline": PIPELINE_VERSION, "region": region,
              "samples": config["samples"], "spacing_metres": config["spacing_metres"],
              "max_zoom": config["max_zoom"], "source": config["source"]}
    return hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()


def reusable(region: dict, config: dict, output: Path):
    directory = output / region["id"]
    try:
        metadata = json.loads((directory / "metadata.json").read_text())
        data = (directory / "height.h16").read_bytes()
        if (metadata["input_key"] == input_key(region, config)
                and len(data) == config["samples"] ** 2 * 2
                and hashlib.sha256(data).hexdigest() == metadata["sha256"]
                and (directory / "preview.png").is_file()
                and (directory / "height.png").is_file()):
            return metadata
    except (OSError, ValueError, KeyError):
        pass
    return None


def prepare(region: dict, config: dict, output: Path, cache: Path) -> dict:
    if (previous := reusable(region, config, output)) is not None:
        return previous
    count = config["samples"]
    spacing = config["spacing_metres"]
    latitude, longitude = region["latitude"], region["longitude"]
    extent = (count - 1) * spacing
    local = CRS.from_proj4(f"+proj=aeqd +lat_0={latitude} +lon_0={longitude} +datum=WGS84 +units=m +no_defs")
    to_geographic = Transformer.from_crs(local, "EPSG:4326", always_xy=True)
    east = (np.arange(count, dtype=np.float64) - (count - 1) / 2) * spacing
    north = -east  # row zero is north; columns increase east
    east, north = np.meshgrid(east, north)
    lon, lat = to_geographic.transform(east, north)
    zoom = source_zoom(latitude, spacing, config["max_zoom"])
    pixels = TILE_SIZE * (1 << zoom)
    x = (lon + 180) / 360 * pixels - 0.5
    y = (1 - np.arcsinh(np.tan(np.radians(lat))) / math.pi) / 2 * pixels - 0.5
    x0, y0 = math.floor(float(x.min())) // TILE_SIZE, math.floor(float(y.min())) // TILE_SIZE
    x1 = (math.floor(float(x.max())) + 1) // TILE_SIZE
    y1 = (math.floor(float(y.max())) + 1) // TILE_SIZE
    if x0 < 0 or y0 < 0 or x1 >= 1 << zoom or y1 >= 1 << zoom:
        raise ValueError("sample crosses the antimeridian or Mercator limit; split the region first")
    if (x1 - x0 + 1) * (y1 - y0 + 1) > 100:
        raise ValueError("sample would request over 100 tiles")
    mosaic = np.empty(((y1 - y0 + 1) * TILE_SIZE, (x1 - x0 + 1) * TILE_SIZE), dtype=np.float32)
    sources = []
    for ty in range(y0, y1 + 1):
        for tx in range(x0, x1 + 1):
            url = config["source"]["url_template"].format(z=zoom, x=tx, y=ty)
            path = cache / str(zoom) / str(tx) / f"{ty}.png"
            data = fetch(url, path, tile=True)
            row, col = (ty - y0) * TILE_SIZE, (tx - x0) * TILE_SIZE
            mosaic[row:row + TILE_SIZE, col:col + TILE_SIZE] = tile_height(data)
            sources.append({"z": zoom, "x": tx, "y": ty, "url": url,
                            "sha256": hashlib.sha256(data).hexdigest()})
    x -= x0 * TILE_SIZE
    y -= y0 * TILE_SIZE
    ix, iy = np.floor(x).astype(np.int32), np.floor(y).astype(np.int32)
    fx, fy = x - ix, y - iy
    height = ((mosaic[iy, ix] * (1 - fx) + mosaic[iy, ix + 1] * fx) * (1 - fy)
              + (mosaic[iy + 1, ix] * (1 - fx) + mosaic[iy + 1, ix + 1] * fx) * fy)
    if not np.isfinite(height).all():
        raise ValueError(f"{region['id']}: source contains missing heights; not filling them with invented terrain")
    low, high = float(height.min()), float(height.max())
    if low < region.get("min_height_metres", -500) or high > region.get("max_height_metres", 9000):
        raise ValueError(f"{region['id']}: implausible terrain heights [{low:.1f}, {high:.1f}] m; reject instead of smoothing source errors")
    if high - low < 30:
        raise ValueError(f"{region['id']}: insufficient relief ({high-low:.1f} m)")
    scale = (high - low) / 65535
    encoded = np.rint((height - low) / scale).clip(0, 65535).astype("<u2")
    payload = encoded.tobytes(order="C")
    dy, dx = np.gradient(height, spacing)
    steepness = np.hypot(dx, dy)
    # Preview only; the height assets themselves have no shading or overlays.
    light = np.clip((0.6 * dx + 0.45 * dy + 0.8) / np.sqrt(1 + dx * dx + dy * dy), 0, 1)
    gray = np.clip(24 + light * 170 + (height - low) / (high - low) * 60, 0, 255).astype(np.uint8)
    preview = Image.fromarray(gray).resize((256, 256), Image.Resampling.LANCZOS)
    metadata = {
        "id": region["id"], "name": region["name"], "family": region["family"],
        "pipeline_version": PIPELINE_VERSION, "input_key": input_key(region, config),
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "width": count, "height": count, "spacing_metres": spacing,
        "extent_between_outer_samples_metres": extent,
        "centre_wgs84": {"latitude": latitude, "longitude": longitude},
        "geographic_bounds": [float(lon.min()), float(lat.min()), float(lon.max()), float(lat.max())],
        "local_crs": local.to_wkt(), "row_order": "north_to_south", "column_order": "west_to_east",
        "local_top_left_sample_metres": [-extent / 2, extent / 2],
        "binary": "height.h16", "format": "uint16_little_endian_row_major_no_header",
        "height_formula": "metres = height_offset_metres + stored_uint16 * height_scale_metres",
        "height_offset_metres": low, "height_scale_metres": scale,
        "min_metres": low, "max_metres": high, "relief_metres": high - low,
        "max_quantisation_error_metres": scale / 2,
        "slope_degrees_p50_p95_p99": np.degrees(np.arctan(np.percentile(steepness, [50, 95, 99]))).tolist(),
        "zero_height_fraction": float(np.mean(np.abs(height) < 0.05)),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "source_zoom": zoom,
        "source_tile_grid_metres_at_centre": 156543.033928 * math.cos(math.radians(latitude)) / (1 << zoom),
        "source_warning": config["source"]["warning"],
        "attribution": "../ATTRIBUTION_UPSTREAM.md", "sources": sources,
    }
    directory = output / region["id"]
    atomic_write(directory / "height.h16", payload)
    atomic_write(directory / "height.png", image_bytes(Image.fromarray(encoded)))
    atomic_write(directory / "preview.png", image_bytes(preview))
    atomic_write(directory / "metadata.json", json_bytes(metadata))  # completion marker, last
    return metadata


def catalogue(config: dict, output: Path) -> dict:
    entries = []
    for region in config["regions"]:
        if (metadata := reusable(region, config, output)) is not None:
            entries.append({k: metadata[k] for k in ("id", "name", "family", "relief_metres", "sha256", "source_zoom")}
                           | {"metadata": f"{region['id']}/metadata.json", "height": f"{region['id']}/height.h16"})
    index = {"pipeline_version": PIPELINE_VERSION, "source": config["source"],
             "expected": len(config["regions"]), "complete": len(entries), "regions": entries}
    atomic_write(output / "index.json", json_bytes(index))
    if entries:
        columns, cell_width, cell_height = 8, 256, 282
        sheet = Image.new("RGB", (columns * cell_width, math.ceil(len(entries) / columns) * cell_height), (24, 24, 24))
        draw = ImageDraw.Draw(sheet)
        for i, entry in enumerate(entries):
            x, y = i % columns * cell_width, i // columns * cell_height
            with Image.open(output / entry["id"] / "preview.png") as preview:
                sheet.paste(preview.convert("RGB"), (x, y))
            draw.text((x + 5, y + 258), entry["id"], fill=(235, 235, 235))
        atomic_write(output / "contact_sheet.jpg", image_bytes(sheet, "JPEG"))
    return index


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=ROOT / "content/config/terrain_references.json")
    parser.add_argument("--output", type=Path, default=ROOT / "assets/generated/terrain_references")
    parser.add_argument("--cache", type=Path, default=ROOT / ".cache/terrain_reference_tiles")
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--only", nargs="+", help="optional region IDs; completed others remain in the catalogue")
    args = parser.parse_args()
    config = json.loads(args.manifest.read_text())
    regions = config["regions"]
    ids = [r["id"] for r in regions]
    if len(ids) != len(set(ids)) or any(not name or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_" for c in name) for name in ids):
        parser.error("region IDs must be unique safe directory names")
    if not 2 <= config["samples"] <= 2048 or not 1 <= config["spacing_metres"] <= 64 or not 1 <= config["max_zoom"] <= 14:
        parser.error("unsupported sample size, spacing or zoom")
    for region in regions:
        if not -80 < region["latitude"] < 80 or not -180 < region["longitude"] < 180:
            parser.error(f"invalid coordinates: {region['id']}")
    if args.only:
        if set(args.only) - set(ids):
            parser.error("unknown region ID")
        regions = [r for r in regions if r["id"] in args.only]
    # Preserve provider attribution and data-source documentation beside assets.
    for field, filename in (("attribution_url", "ATTRIBUTION_UPSTREAM.md"), ("data_sources_url", "DATA_SOURCES_UPSTREAM.md")):
        fetch(config["source"][field], args.output / filename)
    started = time.monotonic()
    failures = []
    print(f"Preparing {len(regions)} references, {config['samples']}x{config['samples']}, {config['spacing_metres']} m grid", flush=True)
    with ThreadPoolExecutor(max_workers=max(1, min(8, args.workers))) as pool:
        futures = {pool.submit(prepare, region, config, args.output, args.cache): region for region in regions}
        for completed, future in enumerate(as_completed(futures), 1):
            region = futures[future]
            try:
                metadata = future.result()
                print(f"[{completed}/{len(regions)}] {region['id']}: relief {metadata['relief_metres']:.1f} m, z{metadata['source_zoom']}", flush=True)
            except Exception as error:
                failures.append({"id": region["id"], "error": str(error)})
                print(f"[{completed}/{len(regions)}] FAILED {region['id']}: {error}", flush=True)
    index = catalogue(config, args.output)
    atomic_write(args.output / "download_report.json", json_bytes({"elapsed_seconds": time.monotonic() - started,
        "requested": len(regions), "successful": len(regions) - len(failures), "failures": failures,
        "catalogue_complete": index["complete"], "catalogue_expected": index["expected"]}))
    print(f"Catalogue: {index['complete']}/{index['expected']} in {args.output}; elapsed {time.monotonic()-started:.1f}s", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())

