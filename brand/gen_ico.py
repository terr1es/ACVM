"""Generate Axiom .ico with per-size optical tuning.

Geometry constants follow brand/axiom-logo-*.png (256-space canvas):
  triangle apex (128,28), base y=228, apex angle 50 deg
  inner A legs parallel to the triangle sides, mitered apex, flat baseline
Only the three stroke parameters vary with the target size.
"""
import math
import os
import struct

import numpy as np
from PIL import Image

WORD = 256                      # design canvas
APEX_Y = 28.0
BASE_Y = 228.0
HALF_ANGLE = math.radians(25.0)
T25 = math.tan(HALF_ANGLE)
S25 = math.sin(HALF_ANGLE)
C25 = math.cos(HALF_ANGLE)
HB = (BASE_Y - APEX_Y) * T25    # half base width = 93.26

TILE = (0x1F, 0x24, 0x30)       # the editor-extension badge navy
TILE_INSET = 8.0
TILE_RADIUS = 44.0

# per-size stroke parameters, in 256-space units
#   S: (outer, inner, gap, crossbar)
PARAMS = {
    16: (18.5, 16.0, 15.0, False),
    20: (16.0, 14.0, 14.5, False),
    24: (14.0, 12.0, 14.0, True),
    32: (12.0, 10.0, 13.5, True),
    40: (11.0, 9.0, 13.0, True),
    48: (10.5, 8.5, 13.0, True),
    64: (10.0, 8.0, 13.0, True),
    96: (10.0, 8.0, 13.0, True),
    128: (10.0, 8.0, 13.0, True),
    256: (10.0, 8.0, 13.0, True),
}


def seg_sdf(px, py, p1, p2):
    dx, dy = p2[0] - p1[0], p2[1] - p1[1]
    L2 = dx * dx + dy * dy
    t = np.clip(((px - p1[0]) * dx + (py - p1[1]) * dy) / L2, 0.0, 1.0)
    return np.hypot(px - (p1[0] + t * dx), py - (p1[1] + t * dy))


def convex_mask(px, py, poly):
    signs = []
    for i in range(len(poly)):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % len(poly)]
        signs.append(np.sign((x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)))
    ref = signs[0]
    m = np.ones(px.shape, bool)
    for s in signs:
        m &= (s == ref) | (s == 0)
    return m


def a_geometry(outer, inner, gap):
    """Returns (outer_tri, holes[]) in 256-space, or None if it collapses."""
    a_top = APEX_Y + (gap + outer / 2.0) / S25
    ht = (inner / 2.0) / S25          # vertical extent of the miter
    ta = a_top + ht                    # centreline apex
    base = BASE_Y - outer / 2.0 - gap
    ia = ta + ht                       # inner apex (where the two legs meet inside)
    if base - a_top < 8.0 or inner * 1.0 / C25 >= (base - a_top) * T25:
        return None
    oh = (base - a_top) * T25          # outer half width at the baseline
    ih = (base - ia) * T25             # inner half width at the baseline
    tri = [(128.0, a_top), (128.0 - oh, base), (128.0 + oh, base)]

    holes = []
    bar_h = inner
    bar_c = a_top + 0.615 * (base - a_top)
    bar_t, bar_b = bar_c - bar_h / 2.0, bar_c + bar_h / 2.0
    ih_t = (bar_t - ia) * T25
    ih_b = (bar_b - ia) * T25
    if ih_t <= 0.0:
        # A too short for a separate upper hole: single opening
        holes.append([(128.0, ia), (128.0 - ih, base), (128.0 + ih, base)])
    else:
        holes.append([(128.0, ia), (128.0 - ih_t, bar_t), (128.0 + ih_t, bar_t)])
        holes.append([(128.0 - ih_b, bar_b), (128.0 + ih_b, bar_b),
                      (128.0 + ih, base), (128.0 - ih, base)])
    return tri, holes


def render_glyph(S, outer, inner, gap, crossbar, sub=6):
    """White-on-alpha glyph, returned as float alpha in [0,1] at S x S."""
    k = WORD / float(S)                     # units per device pixel
    yy, xx = np.mgrid[0:S, 0:S]
    px, py = (xx + 0.5) * k, (yy + 0.5) * k

    tri = [(128.0, APEX_Y), (128.0 - HB, BASE_Y), (128.0 + HB, BASE_Y)]
    d = np.full((S, S), 1e9)
    for i in range(3):
        d = np.minimum(d, seg_sdf(px, py, tri[i], tri[(i + 1) % 3]))
    a_out = np.clip(outer / 2.0 - d + k * 0.5, 0.0, 1.0)

    geo = a_geometry(outer, inner, gap)
    if geo is None:
        return np.clip(a_out, 0, 1)
    atri, holes = geo
    if not crossbar:
        # drop the crossbar: a single opening between the two legs
        ht = (inner / 2.0) / S25
        a_top = APEX_Y + (gap + outer / 2.0) / S25
        base = BASE_Y - outer / 2.0 - gap
        ia = a_top + 2 * ht
        ih = (base - ia) * T25
        holes = [[(128.0, ia), (128.0 - ih, base), (128.0 + ih, base)]]

    acc = np.zeros((S, S), np.float32)
    for sy in range(sub):
        for sx in range(sub):
            qx = px + ((sx + 0.5) / sub - 0.5) * k
            qy = py + ((sy + 0.5) / sub - 0.5) * k
            m = convex_mask(qx, qy, atri)
            for h in holes:
                m &= ~convex_mask(qx, qy, h)
            acc += m.astype(np.float32)
    a_in = acc / float(sub * sub)
    return np.clip(np.maximum(a_out, a_in), 0.0, 1.0)


def tile_alpha(S, inset=TILE_INSET, radius=TILE_RADIUS):
    k = WORD / float(S)
    yy, xx = np.mgrid[0:S, 0:S]
    px, py = (xx + 0.5) * k, (yy + 0.5) * k
    lo, hi = inset, WORD - inset
    cx = np.clip(px, lo + radius, hi - radius)
    cy = np.clip(py, lo + radius, hi - radius)
    d = np.hypot(px - cx, py - cy) - radius
    inside_box = (px >= lo) & (px <= hi) & (py >= lo) & (py <= hi)
    a = np.clip(0.5 - d / k, 0.0, 1.0)
    return np.where(inside_box, a, 0.0)


def render_icon(S, with_tile=True):
    outer, inner, gap, crossbar = PARAMS[S]
    glyph = render_glyph(S, outer, inner, gap, crossbar)
    rgba = np.zeros((S, S, 4), np.uint8)
    if with_tile:
        ta = tile_alpha(S)
        rgba[..., 0], rgba[..., 1], rgba[..., 2] = TILE
        rgba[..., 3] = (ta * 255).round().astype(np.uint8)
        # white glyph composited over the tile
        g = glyph * ta
        for c in range(3):
            rgba[..., c] = (255 * g + rgba[..., c] * (ta - g)).round().clip(0, 255).astype(np.uint8)
        rgba[..., 3] = (np.maximum(ta, g) * 255).round().astype(np.uint8)
    else:
        rgba[..., 0] = rgba[..., 1] = rgba[..., 2] = 12
        rgba[..., 3] = (glyph * 255).round().astype(np.uint8)
    return Image.fromarray(rgba, "RGBA")


def bmp_entry(img):
    """32bpp DIB (BITMAPINFOHEADER + XOR + AND mask) as stored in an ICO."""
    w, h = img.size
    a = np.array(img.convert("RGBA"))
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, w * h * 4, 0, 0, 0, 0)
    xor = np.zeros((h, w, 4), np.uint8)
    xor[..., 0] = a[::-1, :, 2]      # B
    xor[..., 1] = a[::-1, :, 1]      # G
    xor[..., 2] = a[::-1, :, 0]      # R
    xor[..., 3] = a[::-1, :, 3]      # A
    mask_row = ((w + 31) // 32) * 4
    mask = bytes(mask_row * h)       # all zero: alpha channel decides
    return hdr + xor.tobytes() + mask


def png_entry(img):
    import io
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def write_ico(path, sizes=PARAMS.keys(), with_tile=True):
    entries = []
    for S in sorted(sizes):
        img = render_icon(S, with_tile=with_tile)
        data = png_entry(img) if S >= 128 else bmp_entry(img)
        entries.append((S, img, data))

    head = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    dirs, blobs = b"", b""
    for S, img, data in entries:
        w = 0 if S >= 256 else S
        dirs += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    with open(path, "wb") as f:
        f.write(head + dirs + blobs)
    return len(head + dirs + blobs)


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "axiom-glyph.ico")
    n = write_ico(out, with_tile=True)
    print("wrote %s  %d bytes  sizes=%s" % (out, n, sorted(PARAMS)))

    out2 = os.path.join(here, "axiom-glyph-transparent.ico")
    n2 = write_ico(out2, with_tile=False)
    print("wrote %s  %d bytes" % (out2, n2))

    # per-size legibility report
    print("\nS    outer  inner  gap   A height   device pixels (outer/inner/gap)")
    for S in sorted(PARAMS):
        o, i, g, cb = PARAMS[S]
        a_top = APEX_Y + (g + o / 2.0) / S25
        base = BASE_Y - o / 2.0 - g
        print("%-4d %5.1f  %5.1f  %4.1f  %5.1f  %.2f / %.2f / %.2f %s" % (
            S, o, i, g, (base - a_top) * S / WORD,
            o * S / WORD, i * S / WORD, g * S / WORD, "crossbar" if cb else "-"))
