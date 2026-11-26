#!/usr/bin/env python3
"""The README's banner (docs/images/banner.png, 1600x480) in Crema's layout: OrbisWeb's own emblem from its start
picture (art/render/pic1.png, rendered by art/orbisweb_art.py) on a night-blue field, and the title. Original art
only; the text is in Roboto (app/data/fonts, SIL Open Font License / Apache 2.0).

    python3 scripts/make-banner.py
"""
import math
import os
import random

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
W, H = 1600, 480
FIELD_EDGE, FIELD_GLOW = (2, 8, 26), (14, 46, 112)
TEXT_TOP, TEXT_LOW = (240, 246, 255), (128, 184, 255)
GLOW = (70, 130, 255)
MUTED = (170, 192, 232)


def font(size, name="Roboto-Bold.ttf"):
    for path in (os.path.join(ROOT, "app", "data", "fonts", name), "C:/Windows/Fonts/arialbd.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def lerp(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def field(cx, cy):
    """Night blue, brightest behind the emblem, as on the start picture."""
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        for x in range(W):
            d = math.hypot((x - cx) / (W * 0.55), (y - cy) / (H * 0.95))
            px[x, y] = lerp(FIELD_GLOW, FIELD_EDGE, d ** 0.9)
    return img


def emblem(target_h):
    """The ribbons and their light from the start picture, faded out at the crop's edges."""
    pic = Image.open(os.path.join(ROOT, "art", "render", "pic1.png")).convert("RGB")
    sx, sy = pic.width / 3840, pic.height / 2160
    crop = pic.crop((int(1330 * sx), int(230 * sy), int(2450 * sx), int(1520 * sy)))
    scale = target_h / crop.height
    crop = crop.resize((int(crop.width * scale), target_h), Image.LANCZOS)
    mask = Image.new("L", crop.size, 0)
    md = ImageDraw.Draw(mask)
    md.ellipse([crop.width * 0.06, crop.height * 0.02, crop.width * 0.94, crop.height * 0.98], fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(crop.width * 0.08))
    return crop, mask


def bokeh(img, rng):
    """Soft out-of-focus lights, as the start picture has."""
    layer = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(layer)
    placed = 0
    while placed < 12:
        x, y = rng.uniform(0.32, 0.98) * W, rng.uniform(0.05, 0.95) * H
        if 0.45 * W < x < 0.9 * W and 90 < y < 390:   # not behind the title and its subtitle
            continue
        placed += 1
        r = rng.uniform(5, 15)
        colour = rng.choice(((40, 110, 230), (30, 150, 170), (90, 80, 200)))
        d.ellipse([x - r, y - r, x + r, y + r], fill=colour)
    layer = layer.filter(ImageFilter.GaussianBlur(3))
    return ImageChops.lighter(img, layer)


def gradient_text(img, xy, text, f, top, low, glow=None):
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).text(xy, text, font=f, fill=255)
    box = mask.getbbox()
    if glow:
        halo = mask.filter(ImageFilter.GaussianBlur(16))
        halo = ImageChops.multiply(halo, Image.new("L", img.size, glow[1]))
        img = Image.composite(Image.new("RGB", img.size, glow[0]), img, halo)
    fill = Image.new("RGB", img.size)
    fd = ImageDraw.Draw(fill)
    for y in range(box[1], box[3] + 1):
        fd.line([(box[0], y), (box[2], y)], fill=lerp(top, low, (y - box[1]) / max(1, box[3] - box[1])))
    return Image.composite(fill, img, mask)


def main():
    rng = random.Random(7)
    ex = int(W * 0.25)
    img = field(ex, H * 0.45)
    img = bokeh(img, rng)
    art, mask = emblem(H - 30)
    x0, y0 = ex - art.width // 2, 12
    lit = img.crop((x0, y0, x0 + art.width, y0 + art.height))
    lit = Image.composite(ImageChops.lighter(lit, art), lit, mask)
    img.paste(lit, (x0, y0))
    x = int(W * 0.47)
    img = gradient_text(img, (x, 118), "OrbisWeb", font(150), TEXT_TOP, TEXT_LOW, glow=(GLOW, 150))
    d = ImageDraw.Draw(img)
    d.text((x + 8, 300), "A WebKit browser for the PS4", font=font(40, "Roboto-Regular.ttf"), fill=MUTED)
    # A thin rule under the subtitle, fading out to the right.
    for i in range(560):
        d.point((x + 8 + i, 362), fill=lerp((110, 170, 255), FIELD_EDGE, (i / 560) ** 0.8))
    out = os.path.join(ROOT, "docs", "images", "banner.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    img.save(out, optimize=True)
    print("wrote", out)


if __name__ == "__main__":
    main()
