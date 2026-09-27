"""Prepares the installer's character renders (res/installer/characters).

The renders are SEGA's official Sonic the Fighters artwork, from the Sonic Wiki
Zone (https://sonic.fandom.com/wiki/Category:Sonic_the_Fighters_stock_artwork).
Some have a transparent background, the others a white one, which is keyed out
here: a flood fill of pure white from the edges, with the edge pixels unmixed
from the white. White faces touching the background can't be told apart from
it, so shapes traced by hand are kept.

Usage: python key_characters.py <downloaded renders dir> <output dir>
Needs numpy, scipy and Pillow.
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

# Output name: wiki file name.
SOURCES = {
    "sonic": "Sonic 14.png",
    "tails": "Tails 7.png",
    "knuckles": "Knuckles-6.png",
    "amy": "Amy 4.png",
    "espio": "Espio Sonic Fighters.png",
    "fang": "Knack 1.png",
    "bean": "Chara bean.png",
    "bark": "Bark 1.png",
    "metal_sonic": "Metal Sonic the Fighter.png",
    "robotnik": "Robotnik 52.png",
}

# Shapes to keep (source pixels) and background seeds for white regions the
# kept shapes enclose.
KEEP = {
    "tails": [
        # Glove.
        [(320, 1122), (385, 1128), (390, 1190), (460, 1225), (485, 1295), (455, 1340),
         (410, 1410), (370, 1420), (265, 1375), (260, 1330), (275, 1240), (300, 1200),
         (305, 1185)],
        # Socks.
        [(505, 1398), (615, 1385), (650, 1535), (510, 1522)],
        [(705, 1403), (800, 1362), (855, 1468), (750, 1515), (712, 1480)],
        # Tail tips.
        [(35, 1440), (420, 1600), (190, 1720), (80, 1680)],
        [(265, 1385), (300, 1400), (375, 1497), (262, 1485), (222, 1570), (205, 1545)],
    ],
}
BACKGROUND_SEEDS = {
    "tails": [(675, 1300)],  # between the legs
}

MAX_SIZE = 1024


def key_white(name, rgb):
    """Alpha for a render on white."""
    white = (rgb >= 250).all(-1)
    keep = np.zeros(white.shape, bool)
    if name in KEEP:
        mask = Image.new("L", (white.shape[1], white.shape[0]), 0)
        draw = ImageDraw.Draw(mask)
        for polygon in KEEP[name]:
            draw.polygon(polygon, fill=255)
        keep = np.asarray(mask) > 0
    labels, _ = ndimage.label(white & ~keep)
    seeds = set(np.unique(np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]])))
    seeds |= {labels[y, x] for x, y in BACKGROUND_SEEDS.get(name, [])}
    seeds.discard(0)
    foreground = ~np.isin(labels, list(seeds))
    # Unmix the edge from the white using the nearest interior colour.
    interior = ndimage.distance_transform_edt(foreground) >= 2.5
    _, (iy, ix) = ndimage.distance_transform_edt(~interior, return_indices=True)
    inner = rgb[iy, ix]
    d = 255.0 - inner
    denom = (d * d).sum(-1)
    alpha = np.where(denom > 100, ((255.0 - rgb) * d).sum(-1) / np.maximum(denom, 1e-3), 1.0)
    alpha = np.where(interior, 1.0, np.where(foreground, np.clip(alpha, 0, 1), 0.0))
    colour = np.where(interior[..., None], rgb, inner)
    return colour, alpha


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    for name, file in SOURCES.items():
        image = np.asarray(Image.open(src / file).convert("RGBA")).astype(np.float32)
        rgb, alpha = image[..., :3], image[..., 3] / 255.0
        if alpha.min() == 1.0:
            rgb, alpha = key_white(name, rgb)
        ys, xs = np.nonzero(alpha > 0.03)
        rgba = np.dstack([rgb, alpha * 255.0])[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
        result = Image.fromarray(np.clip(rgba + 0.5, 0, 255).astype(np.uint8), "RGBA")
        scale = min(1.0, MAX_SIZE / max(result.size))
        if scale < 1.0:
            # Premultiplied, so the transparent pixels' colour doesn't bleed in.
            size = (max(1, round(result.width * scale)), max(1, round(result.height * scale)))
            result = result.convert("RGBa").resize(size, Image.LANCZOS).convert("RGBA")
        result.save(out / f"{name}.png", optimize=True)
        print(name, result.size)


if __name__ == "__main__":
    main()
