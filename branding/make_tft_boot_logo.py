# -*- coding: utf-8 -*-
"""Render the OSHI mark + wordmark as the TFT boot-splash PNG this directory's
own README documents: `branding/logo_<width>x<height>.png`, read by
`bin/platformio-custom.py`'s `load_boot_logo()` for any `HAS_TFT=1` variant and
copied verbatim to `data/boot/logo.png` before the filesystem image is built.

This is the colour counterpart of `OSHI/LoRaFirmware/tools/oshi_boot_logo.py`
in the app repo, which draws the monochrome XBM `USERPREFS_OEM_IMAGE_DATA` used
by the OLED/E-Ink path. That script's own header explains why neither of these
is downscaled from the 1024x1024 App Store icon: at boot-screen sizes a resized
source image turns to mush, so both are drawn as geometry instead.

The file is written at the PANEL'S OWN pixel size (no implicit scaling by the
firmware side - that is the entire point of the `logo_<w>x<h>.png` naming
convention), so getting the size argument right for a given variant is what
makes this correct, not any property of the drawing code.
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# Sampled from OSHI/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png: a
# diagonal gradient from light purple (top-left) to dark purple (bottom-right),
# with a white ring + tail mark on top. Reproduced here rather than scaling the
# PNG so the mark stays crisp at small boot-screen sizes.
TOP_LEFT = (121, 59, 254)
BOTTOM_RIGHT = (62, 15, 177)
MARK_COLOR = (255, 255, 255)


def gradient_background(w: int, h: int) -> Image.Image:
    img = Image.new("RGB", (w, h))
    px = img.load()
    # Diagonal gradient: blend factor is each pixel's position along the
    # top-left -> bottom-right diagonal, normalized to the longer axis so it
    # reaches full saturation at the far corner on any aspect ratio.
    denom = max(w + h - 2, 1)
    for y in range(h):
        for x in range(w):
            t = (x + y) / denom
            r = int(TOP_LEFT[0] + (BOTTOM_RIGHT[0] - TOP_LEFT[0]) * t)
            g = int(TOP_LEFT[1] + (BOTTOM_RIGHT[1] - TOP_LEFT[1]) * t)
            b = int(TOP_LEFT[2] + (BOTTOM_RIGHT[2] - TOP_LEFT[2]) * t)
            px[x, y] = (r, g, b)
    return img


def load_font(size: int):
    for path in [
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    ]:
        try:
            font = ImageFont.truetype(path, size)
            try:
                font.set_variation_by_axes([100, 20, 400, 800])  # heavy
            except Exception:
                pass
            return font
        except Exception:
            continue
    return ImageFont.load_default()


def draw_mark(d: ImageDraw.ImageDraw, cx: int, cy: int, r_out: int, r_in: int):
    """The app icon's ring with its speech-bubble tail, as geometry."""
    d.ellipse([cx - r_out, cy - r_out, cx + r_out, cy + r_out], fill=MARK_COLOR)
    d.ellipse(
        [cx - r_in, cy - r_in, cx + r_in, cy + r_in],
        fill=TOP_LEFT if r_in > 0 else MARK_COLOR,
    )
    tail_dx = r_out * 0.62
    tail_dy = r_out * 0.54
    d.polygon(
        [
            (cx - tail_dx, cy + tail_dy),
            (cx - tail_dx * 0.3, cy + tail_dy * 1.3),
            (cx - tail_dx * 0.8, cy + tail_dy * 2.4),
        ],
        fill=MARK_COLOR,
    )


def render(width: int, height: int) -> Image.Image:
    img = gradient_background(width, height)
    d = ImageDraw.Draw(img)

    # Mark sized off the SHORTER side so a wide landscape panel (320x240)
    # doesn't force a mark too tall to clear the wordmark's cap-height, and a
    # tall portrait panel doesn't force one too wide to leave side margins.
    short_side = min(width, height)
    r_out = short_side * 0.17
    r_in = r_out * 0.54
    cx = width * 0.5
    cy = height * 0.42

    font_size = int(short_side * 0.17)
    font = load_font(font_size)

    # Measure before placing - the OLED script's own history (see its header
    # comment) is that a hard-coded position clipped the "I" once and collided
    # with the ring once. Centering the mark+wordmark pair as a unit avoids
    # both: there is no fixed x to go stale if the mark's radius changes.
    tmp = Image.new("RGB", (1, 1))
    tw = ImageDraw.Draw(tmp).textlength("OSHI", font=font)
    gap = short_side * 0.08
    group_w = r_out * 2 + gap + tw
    mark_cx = width / 2 - group_w / 2 + r_out
    text_x = mark_cx + r_out + gap

    draw_mark(d, mark_cx, cy, r_out, r_in)
    ascent, descent = font.getmetrics()
    d.text((text_x, cy - ascent / 2), "OSHI", font=font, fill=MARK_COLOR)

    return img


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <width> <height>  (e.g. 320 240)")
    width, height = int(sys.argv[1]), int(sys.argv[2])
    out_path = Path(__file__).parent / f"logo_{width}x{height}.png"
    render(width, height).save(out_path)
    print(f"wrote {out_path} ({width}x{height})")


if __name__ == "__main__":
    main()
