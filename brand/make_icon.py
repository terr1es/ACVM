#!/usr/bin/env python3
"""Render the Axiom file-type icon (multi-size .ico) from the logo geometry.

The geometry below is not invented: every constant was recovered by scanning
`brand/axiom-logo-256.png` row by row and column by column.  Keeping the
numbers here instead of always re-deriving them from the PNG lets each ICO
entry be rendered at its own resolution with size-specific optical
corrections, which a plain downscale cannot do.

Design, matching vscode-axiom/icons/extension-icon.png (measured: the badge
fills the canvas, #1F2430, corner radius 14.8% of the side, white logo whose
bbox is 56% of the side wide and centred):

    16 .. 32 px   solid triangle silhouette
    40 .. 64 px   bold outline (thicker strokes, logo fills more of the badge)
    128 px        intermediate
    256 px        the real logo: same proportions as the .vsix icon

The reductions are not laziness: measured at 32 px, a faithful outline gives a
0.9 px triangle stroke and a 0.7 px A stroke, which antialiases into grey mush
(see the 32 px column of `_probe.py` experiments in the project history).  Icon
families routinely swap in a simplified mark at those sizes; a solid triangle
stays recognisable at 16 px, a hairline outline does not.

Usage:  python brand/make_icon.py [--out DIR]
"""

from __future__ import annotations

import math
import os
import sys

from PIL import Image, ImageDraw

# --------------------------------------------------------------------------
# Geometry, in the 256x256 design space
# --------------------------------------------------------------------------

CENTER_X = 128.0

# Outer triangle: apex (128, 28), base y = 228, apex angle 50 degrees.
APEX_Y = 28.0
BASE_Y = 228.0
HALF_BASE = 200.0 * math.tan(math.radians(25.0))     # 93.2615316
TRI_STROKE = 10.0

# Inner A: legs parallel to the triangle legs, joined by a crossbar.
A_APEX_Y = 70.59      # tip of the A (sharp miter, not rounded)
A_NOTCH_Y = 89.52     # underside of the tip, where the two inner edges meet
A_BASE_Y = 210.0      # flat cut at the baseline
A_BAR_TOP = 152.33
A_BAR_BOTTOM = 160.33
A_STROKE = 8.0

# dx/dy of a leg: the left leg runs from (128, 28) to (128 - 93.26, 228).
LEG_SLOPE = HALF_BASE / (BASE_Y - APEX_Y)            # 0.4663077

BADGE_COLOR = (31, 36, 48, 255)                      # #1F2430
INK_COLOR = (255, 255, 255, 255)
PLAIN_INK_COLOR = (12, 12, 14, 255)                  # #0C0C0E, the brand ink
BADGE_RADIUS_RATIO = 19.0 / 128.0                    # measured on the .vsix icon
FULL_LOGO_SCALE = 0.73                               # measured: bbox = 56% of side

# One parameter set per optical tier.  `scale` maps design space onto the
# badge; `tri` / `a` are stroke widths in design units (the silhouette uses a
# wide band purely to round the corners of the solid mark); `floor_*` is a
# minimum stroke in *final* pixels.
TIERS = {
    "silhouette": dict(scale=1.00, tri=26.0, a=None, floor_tri=0.0, floor_a=0.0),
    "bold":       dict(scale=0.80, tri=13.0, a=11.0, floor_tri=1.2, floor_a=1.1),
    "mid":        dict(scale=0.76, tri=11.5, a=9.5, floor_tri=1.0, floor_a=0.9),
    "full":       dict(scale=FULL_LOGO_SCALE, tri=TRI_STROKE, a=A_STROKE,
                       floor_tri=0.0, floor_a=0.0),
}

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def tier_for(size: int) -> str:
    if size <= 32:
        return "silhouette"
    if size <= 64:
        return "bold"
    if size <= 128:
        return "mid"
    return "full"


def outer_half(y: float) -> float:
    return LEG_SLOPE * (y - A_APEX_Y)


def inner_half(y: float) -> float:
    return LEG_SLOPE * (y - A_NOTCH_Y)


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------

def _supersample(size: int) -> int:
    """Pick a supersampling factor so the working canvas lands near 400-1000 px."""
    for ss in (16, 12, 10, 8, 6, 4):
        if size * ss <= 1100:
            return ss
    return 4


def render(size: int, tier: str | None = None, badge: bool = True) -> Image.Image:
    params = TIERS[tier or tier_for(size)]
    scale = params["scale"]

    ss = _supersample(size)
    canvas = size * ss
    k = canvas / 256.0               # design space -> working canvas

    plate = Image.new("L", (canvas, canvas), 0)
    if badge:
        ImageDraw.Draw(plate).rounded_rectangle(
            [0, 0, canvas - 1, canvas - 1],
            radius=BADGE_RADIUS_RATIO * canvas,
            fill=255,
        )

    ink = Image.new("L", (canvas, canvas), 0)
    draw = ImageDraw.Draw(ink)

    def px(x: float) -> float:       # design x -> canvas x, about the centre
        return (CENTER_X + (x - CENTER_X) * scale) * k

    def py(y: float) -> float:
        return (CENTER_X + (y - CENTER_X) * scale) * k

    def stroke(design: float, floor_px: float) -> int:
        """Stroke width in canvas pixels, with an optical floor at final size."""
        return max(1, int(round(design * scale * k)), int(round(floor_px * ss)))

    def widest(design: float, floor_px: float) -> float:
        """Same floor, but returned in design units so geometry can use it."""
        floored = floor_px * 256.0 / (size * scale) if floor_px > 0 else 0.0
        return max(design, floored)

    # --- outer triangle -------------------------------------------------
    tri = stroke(params["tri"], params["floor_tri"])
    centre = [
        (px(CENTER_X - HALF_BASE), py(BASE_Y)),
        (px(CENTER_X), py(APEX_Y)),
        (px(CENTER_X + HALF_BASE), py(BASE_Y)),
    ]
    draw.line(centre, fill=255, width=tri, joint="curve")
    draw.line([centre[0], centre[2]], fill=255, width=tri)
    for x, y in (centre[0], centre[2]):
        r = tri / 2.0
        draw.ellipse([x - r, y - r, x + r, y + r], fill=255)

    if params["a"] is None:
        # Solid silhouette: fill the band's interior so the mark is one shape.
        draw.polygon(centre, fill=255)
        return _finish(plate, ink, size, badge)

    # --- inner A --------------------------------------------------------
    # Everything about the A follows from one number: `a_width`, the stroke
    # width measured *perpendicular* to a leg.  A leg runs at a slope of
    # LEG_SLOPE, so a horizontal shift of s covers only s / |d| perpendicular
    # distance (|d| = hypot(LEG_SLOPE, 1) = 1.1034); widening the stroke by w
    # therefore moves each edge sideways by w * |d|.
    a_width = widest(params["a"], params["floor_a"])
    edge_shift = a_width * math.hypot(LEG_SLOPE, 1.0)
    notch_y = A_APEX_Y + edge_shift / LEG_SLOPE
    bar_mid = (A_BAR_TOP + A_BAR_BOTTOM) / 2.0
    bar_top = bar_mid - a_width / 2.0
    bar_bottom = bar_mid + a_width / 2.0
    # Painting the A at its own weight keeps the crossbar proportional: the
    # bar is exactly one stroke thick, so it thickens with the legs.

    def outer_w(y: float) -> float:
        return LEG_SLOPE * (y - A_APEX_Y)

    def inner_w(y: float) -> float:
        return LEG_SLOPE * (y - notch_y)

    draw.polygon(
        [
            (px(CENTER_X), py(A_APEX_Y)),
            (px(CENTER_X - outer_w(A_BASE_Y)), py(A_BASE_Y)),
            (px(CENTER_X + outer_w(A_BASE_Y)), py(A_BASE_Y)),
        ],
        fill=255,
    )
    # Two holes, above and below the crossbar.  They never overlap, so simply
    # painting them back to 0 is equivalent to an even-odd fill.
    draw.polygon(
        [
            (px(CENTER_X), py(notch_y)),
            (px(CENTER_X - inner_w(bar_top)), py(bar_top)),
            (px(CENTER_X + inner_w(bar_top)), py(bar_top)),
        ],
        fill=0,
    )
    draw.polygon(
        [
            (px(CENTER_X - inner_w(A_BASE_Y)), py(A_BASE_Y)),
            (px(CENTER_X + inner_w(A_BASE_Y)), py(A_BASE_Y)),
            (px(CENTER_X + inner_w(bar_bottom)), py(bar_bottom)),
            (px(CENTER_X - inner_w(bar_bottom)), py(bar_bottom)),
        ],
        fill=0,
    )
    return _finish(plate, ink, size, badge)


def _finish(plate: Image.Image, ink: Image.Image, size: int, badge: bool) -> Image.Image:
    if badge:
        image = Image.new("RGBA", ink.size, BADGE_COLOR)
        image.paste(Image.new("RGBA", ink.size, INK_COLOR), (0, 0), ink)
    else:
        # Plain variant: the brand ink on nothing, for anyone who would rather
        # keep the bare logo.  Only sensible on a light background.
        image = Image.new("RGBA", ink.size, (0, 0, 0, 0))
        image.paste(Image.new("RGBA", ink.size, PLAIN_INK_COLOR), (0, 0), ink)
    image = image.resize((size, size), Image.LANCZOS)
    return _mirror(image)


def _mirror(image: Image.Image) -> Image.Image:
    """Force exact bilateral symmetry: the foot of a diagonal rasteriser is not
    symmetric, and asymmetry is visible on a logo this geometric."""
    w, h = image.size
    half = w // 2
    left = image.crop((0, 0, half, h)).transpose(Image.FLIP_LEFT_RIGHT)
    out = image.copy()
    out.paste(left, (w - half, 0))
    return out


# --------------------------------------------------------------------------
# ICO container
# --------------------------------------------------------------------------

def _bmp_entry(image: Image.Image) -> bytes:
    """32bpp bottom-up DIB + AND mask, the layout ICO expects for sizes <= 64.

    PNG entries are only safe from Windows Vista on, and some shell paths and
    installers still choke on a PNG for 16/32 px, so the small entries stay BMP.
    """
    import struct

    w, h = image.size
    px = image.convert("RGBA").load()
    header = struct.pack(
        "<IiiHHIIiiII",
        40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0,
    )
    rows = []
    mask_rows = []
    for y in range(h - 1, -1, -1):
        row = bytearray()
        mask = bytearray()
        acc = 0
        for x in range(w):
            r, g, b, a = px[x, y]
            row += bytes((b, g, r, a))
            if a < 128:
                acc |= 1 << (7 - (x % 8))
            if x % 8 == 7:
                mask.append(acc)
                acc = 0
        if w % 8:
            mask.append(acc)
        while len(mask) % 4:
            mask.append(0)
        rows.append(bytes(row))
        mask_rows.append(bytes(mask))
    return header + b"".join(rows) + b"".join(mask_rows)


def _png_entry(image: Image.Image) -> bytes:
    import io

    buffer = io.BytesIO()
    image.save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


def write_ico(path: str, frames: dict[int, Image.Image]) -> int:
    import struct

    entries = []
    for size in sorted(frames):
        payload = _bmp_entry(frames[size]) if size <= 64 else _png_entry(frames[size])
        entries.append((size, payload))

    header = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    directory = bytearray()
    blobs = bytearray()
    for size, payload in entries:
        directory += struct.pack(
            "<BBBBHHII",
            size if size < 256 else 0,
            size if size < 256 else 0,
            0, 0, 1, 32, len(payload), offset,
        )
        blobs += payload
        offset += len(payload)
    with open(path, "wb") as handle:
        handle.write(header + bytes(directory) + bytes(blobs))
    return os.path.getsize(path)


def main(argv: list[str]) -> int:
    out_dir = os.path.dirname(os.path.abspath(__file__))
    if "--out" in argv:
        out_dir = argv[argv.index("--out") + 1]

    frames = {size: render(size) for size in SIZES}
    size_bytes = write_ico(os.path.join(out_dir, "axiom.ico"), frames)
    print(f"wrote {os.path.join(out_dir, 'axiom.ico')} ({size_bytes} bytes)")

    plain = {size: render(size, badge=False) for size in SIZES}
    plain_bytes = write_ico(os.path.join(out_dir, "axiom-plain.ico"), plain)
    print(f"wrote {os.path.join(out_dir, 'axiom-plain.ico')} ({plain_bytes} bytes)")

    # A strip so every size can be judged on both Explorer backgrounds.
    pad = 8
    strip_w = sum(SIZES) + pad * (len(SIZES) + 1)
    row_h = 256 + pad
    strip_h = row_h * 4 + pad
    strip = Image.new("RGBA", (strip_w, strip_h), (255, 255, 255, 255))
    strip.paste(Image.new("RGBA", (strip_w, row_h), (32, 32, 32, 255)), (0, row_h))
    strip.paste(Image.new("RGBA", (strip_w, row_h), (245, 246, 248, 255)), (0, row_h * 2))
    strip.paste(Image.new("RGBA", (strip_w, row_h), (24, 25, 28, 255)), (0, row_h * 3))
    rows = [frames, frames, plain, plain]
    for row, source in enumerate(rows):
        x = pad
        for size in SIZES:
            strip.paste(source[size], (x, row * row_h + 256 - size), source[size])
            x += size + pad
    preview = os.path.join(out_dir, "file-icon-preview.png")
    strip.save(preview)
    print(f"wrote {preview}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
