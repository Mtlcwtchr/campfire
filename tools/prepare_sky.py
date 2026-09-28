#!/usr/bin/env python3
"""Turn the UE sky export (tools/export_ue_sky.py) into runtime sky assets.

    python3 tools/prepare_sky.py [/tmp/campfire_ue_sky] [assets/generated/sky]

Writes, atomically (temporary directory, then rename):
  sky_panorama.png  2048x1024 long-lat, display-referred (the renderer is UNORM,
                    not sRGB): filmic-ish exposure to the HDRI's own median sky.
  cloud_weather.png the MW volumetric cloud weather map (coverage/type).
  cloud_noise.png   GoodSky smooth noise, tiled detail for the cloud density.
  sky.json          what was used, exposure and the horizon/zenith averages.
"""
import json
import shutil
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image


def read_hdr(path):
    data = path.read_bytes()
    end = data.index(b"\n\n") + 2
    header = data[:end].decode("ascii", "replace")
    line_end = data.index(b"\n", end)
    size = data[end:line_end].decode().split()
    if size[0] != "-Y" or size[2] != "+X":
        raise ValueError("unsupported HDR orientation: " + " ".join(size))
    height, width = int(size[1]), int(size[3])
    body = np.frombuffer(data, dtype=np.uint8, offset=line_end + 1)
    if body.size == width * height * 4 and not (body[0] == 2 and body[1] == 2):
        rgbe = body.reshape(height, width, 4)
    else:  # new-style RLE scanlines
        rgbe = np.empty((height, width, 4), np.uint8)
        at = 0
        for y in range(height):
            if body[at] != 2 or body[at + 1] != 2:
                raise ValueError("unsupported HDR scanline encoding")
            at += 4
            for channel in range(4):
                x = 0
                while x < width:
                    count = int(body[at]); at += 1
                    if count > 128:
                        count -= 128
                        rgbe[y, x:x + count, channel] = body[at]; at += 1
                    else:
                        rgbe[y, x:x + count, channel] = body[at:at + count]; at += count
                    x += count
    if "FORMAT=32-bit_rle_rgbe" not in header and "#?RADIANCE" not in header:
        raise ValueError("not a Radiance RGBE file")
    exponent = np.where(rgbe[..., 3] == 0, 0.0, np.ldexp(1.0, rgbe[..., 3].astype(np.int32) - 136))
    return rgbe[..., :3].astype(np.float32) * exponent[..., None].astype(np.float32)


def box(image, factor):
    h, w = image.shape[0] // factor, image.shape[1] // factor
    return image[:h * factor, :w * factor].reshape(h, factor, w, factor, -1).mean(axis=(1, 3))


def main():
    source = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/campfire_ue_sky")
    target = Path(sys.argv[2] if len(sys.argv) > 2 else "assets/generated/sky")
    report = json.loads((source / "sky_export.json").read_text())
    hdri = next((source / e["file"] for e in report if e.get("ok") and e["file"].startswith("goegap")), None)
    if hdri is None:
        hdri = next(source / e["file"] for e in report if e.get("ok") and e["file"].endswith(".hdr"))
    radiance = read_hdr(hdri)
    factor = max(1, radiance.shape[1] // 2048)
    radiance = box(radiance, factor)
    luminance = radiance @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    upper = luminance[: luminance.shape[0] // 2]
    # The sun is a few pixels; the sky around it decides the exposure.
    exposure = 0.55 / max(float(np.percentile(upper, 50)), 1e-6)
    mapped = 1.0 - np.exp(-radiance * exposure)
    display = np.clip(mapped, 0, 1) ** (1 / 2.2)
    staging = Path(tempfile.mkdtemp(prefix="sky-", dir=str(target.parent)))
    try:
        Image.fromarray((display * 255 + 0.5).astype(np.uint8), "RGB").save(staging / "sky_panorama.png")
        with Image.open(source / "TEX_MW_CloudWeatherA.png") as weather:
            weather.convert("RGBA").resize((256, 256), Image.LANCZOS).save(staging / "cloud_weather.png")
        with Image.open(source / "T_GoodSky_noise_smooth.png") as noise:
            noise.convert("RGBA").resize((256, 256), Image.LANCZOS).save(staging / "cloud_noise.png")
        # GoodSky's cloud dome: a top-down hemisphere of cloud masks. The sky
        # uses its red channel as the high cirrus layer above the deck.
        with Image.open(source / "T_GoodSky_clouds_sphere.png") as dome:
            dome.convert("RGBA").resize((1024, 1024), Image.LANCZOS).save(staging / "cloud_dome.png")
        rows = display.shape[0]
        meta = {"source": "AncientSettlement UE export (" + hdri.name + ")",
                "license": "Original pack licenses apply; not CC0.",
                "exposure": exposure, "width": int(display.shape[1]), "height": int(rows),
                "horizon": display[rows // 2 - 8: rows // 2].mean(axis=(0, 1)).round(4).tolist(),
                "zenith": display[:16].mean(axis=(0, 1)).round(4).tolist()}
        (staging / "sky.json").write_text(json.dumps(meta, indent=2))
        if target.exists():
            shutil.rmtree(target)
        staging.chmod(0o755)
        staging.rename(target)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    print("wrote", target, meta)


main()

