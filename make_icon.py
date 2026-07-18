"""Generate the app icon (native/chuhaister.ico) — run once; the .ico is committed.

A dark rounded tile with the swap glyph in Claude-ish green, matching the
tray's own drawn icon so the app looks like one thing everywhere.
"""
from PIL import Image, ImageDraw, ImageFont

SIZES = [16, 24, 32, 48, 64, 128, 256]
BG = (30, 30, 30, 255)
GREEN = (63, 185, 80, 255)


def font_for(px):
    for name in ("segoeuib.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, px)
        except OSError:
            continue
    return ImageFont.load_default()


def render(size):
    # supersample for clean edges at small sizes
    s = size * 4
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = int(s * 0.22)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=r, fill=BG)

    # two arrows pointing opposite ways = "swap"
    w = max(2, int(s * 0.055))
    y1, y2 = int(s * 0.40), int(s * 0.60)
    x0, x1 = int(s * 0.22), int(s * 0.78)
    head = int(s * 0.09)
    d.line([(x0, y1), (x1, y1)], fill=GREEN, width=w)
    d.line([(x1, y1), (x1 - head, y1 - head)], fill=GREEN, width=w)
    d.line([(x1, y1), (x1 - head, y1 + head)], fill=GREEN, width=w)
    d.line([(x0, y2), (x1, y2)], fill=GREEN, width=w)
    d.line([(x0, y2), (x0 + head, y2 - head)], fill=GREEN, width=w)
    d.line([(x0, y2), (x0 + head, y2 + head)], fill=GREEN, width=w)
    return img.resize((size, size), Image.LANCZOS)


# Save from the LARGEST render: PIL derives the smaller entries from the base
# image, so handing it the 16px one would produce a single-size .ico.
render(max(SIZES)).save("chuhaister.ico", format="ICO", sizes=[(s, s) for s in SIZES])
print("wrote chuhaister.ico:", SIZES)
