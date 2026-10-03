#!/usr/bin/env python3
"""Flower cards of real species, composed from the simplified flower models.

    python3 tools/make_flower_cards.py

Each card is a 256 px tuft of several plants of one species, taken from the
ring views of that model's impostor (assets/generated/simplified_models/<id>/
impostors/<variant>/colour-0..7.png), cut to their alpha, stood on one line and
overlapped. Written beside the generated grass cards and appended to cards.json
(layers 16 onwards - assets/shaders/foliage.hlsl kCardFlowers*).
"""
import json
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SIMPLE = ROOT / "assets/generated/simplified_models"
CARDS = ROOT / "assets/generated/foliage_cards"
# card name -> (model, plants on a card, size of each as a share of the card)
SPECIES = {
    "flowers_pink.png": ("periwinkle_plant", 6, 0.50),
    "flowers_lilac.png": ("flower_heliophila", 5, 0.62),
    "flowers_orange.png": ("flower_gazania", 6, 0.50),
    "flowers_gold.png": ("celandine_01", 8, 0.38),
    "flowers_dandelion.png": ("dandelion_01", 6, 0.50),
    "flowers_daisy.png": ("flower_ursinia", 6, 0.52),
}


def views(model):
    out = []
    for variant in sorted((SIMPLE / model / "impostors").glob("v*")):
        for k in range(8):
            f = variant / ("colour-%d.png" % k)
            if f.exists():
                out.append(Image.open(f).convert("RGBA"))
    return out


def cut(im):
    box = im.getchannel("A").point(lambda a: 255 if a > 24 else 0).getbbox()
    return im.crop(box) if box else im


def card(model, count, size, seed):
    rng = np.random.default_rng(seed)
    pool = [cut(v) for v in views(model)]
    canvas = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    for k in range(count):
        plant = pool[int(rng.integers(len(pool)))]
        if rng.random() < 0.5:
            plant = plant.transpose(Image.FLIP_LEFT_RIGHT)
        height = int(256 * size * rng.uniform(0.75, 1.15))
        width = max(8, int(plant.width * height / plant.height))
        if width > 140:
            width, height = 140, int(plant.height * 140 / plant.width)
        plant = plant.resize((width, height), Image.LANCZOS)
        x = int(8 + (240 - width) * (k + rng.uniform(0, 0.8)) / count)
        canvas.alpha_composite(plant, (min(x, 256 - width), 256 - height - int(rng.integers(0, 6))))
    return canvas


def main():
    info = json.loads((CARDS / "cards.json").read_text())
    names = [n for n in info["cards"] if n not in SPECIES]
    for k, (name, (model, count, size)) in enumerate(SPECIES.items()):
        if not (SIMPLE / model / "impostors").exists():
            print("missing", model)
            continue
        card(model, count, size, 100 + k).save(CARDS / name)
        names.append(name)
        print("wrote", name, "from", model)
    info["cards"] = names
    (CARDS / "cards.json").write_text(json.dumps(info, indent=2) + "\n")
    print("layers:", len(names), "cards from layer", info["first_layer"])


if __name__ == "__main__":
    main()
