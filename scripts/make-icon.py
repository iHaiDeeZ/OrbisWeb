#!/usr/bin/env python3
"""Wisp: the package's icon (sce_sys/icon0.png, 512x512) and its start screen (sce_sys/pic1.png,
1920x1080). An original drawing: a wisp of light - three soft strands curling up from a glowing point -
on a night-blue field, with the name under it.

    python3 make-icon.py <app dir>
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

app = Path(sys.argv[1])
out = app / "sce_sys"
out.mkdir(parents=True, exist_ok=True)
font_path = app / "data" / "fonts" / "Roboto-Bold.ttf"


def background(w, h):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            # a dark field, lighter toward the centre-top
            dx, dy = (x - w * 0.5) / w, (y - h * 0.38) / h
            d = math.sqrt(dx * dx + dy * dy)
            t = max(0.0, 1.0 - d * 1.6)
            px[x, y] = (int(10 + 24 * t), int(14 + 34 * t), int(32 + 62 * t))
    return img


def wisp(size, cx, cy, scale):
    """Three strands of light rising from (cx, cy), drawn soft (glow) then sharp."""
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    colours = [(120, 220, 255), (150, 160, 255), (110, 255, 210)]
    for k, colour in enumerate(colours):
        phase = k * 2.1
        points = []
        for i in range(120):
            t = i / 119
            y = cy - t * 260 * scale
            x = cx + math.sin(t * 5.2 + phase) * (18 + 70 * t) * scale * (1 - 0.35 * k / 2)
            points.append((x, y))
        for i in range(len(points) - 1):
            t = i / (len(points) - 1)
            width = max(1, int((16 - 13 * t) * scale))
            alpha = int(255 * (1 - t) ** 0.6)
            draw.line([points[i], points[i + 1]], fill=colour + (alpha,), width=width)
    glow = layer.filter(ImageFilter.GaussianBlur(14 * scale))
    core = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(core).ellipse([cx - 26 * scale, cy - 26 * scale, cx + 26 * scale, cy + 26 * scale],
                                 fill=(230, 250, 255, 255))
    core = core.filter(ImageFilter.GaussianBlur(9 * scale))
    out = Image.alpha_composite(glow, glow)
    out = Image.alpha_composite(out, layer)
    return Image.alpha_composite(out, core)


def compose(w, h, cx, cy, scale, title_px, title_y):
    img = background(w, h).convert("RGBA")
    img = Image.alpha_composite(img, wisp((w, h), cx, cy, scale))
    draw = ImageDraw.Draw(img)
    font = ImageFont.truetype(str(font_path), title_px)
    text = "Wisp"
    tw = draw.textlength(text, font=font)
    draw.text(((w - tw) / 2, title_y), text, font=font, fill=(235, 245, 255, 255))
    return img.convert("RGB")


compose(512, 512, 256, 330, 1.0, 74, 380).save(out / "icon0.png")
compose(1920, 1080, 960, 640, 1.9, 120, 760).save(out / "pic1.png")
print("icons:", out / "icon0.png", out / "pic1.png")
