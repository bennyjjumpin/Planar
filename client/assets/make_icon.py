"""Generates Planar's logo as planar.ico (16-256 px), planar.icns (macOS,
16-1024 px) and planar.svg.

The logo is two flat isometric planes, one stacked above the other. It's
pure geometry, so this rasterizes it directly (4x4 supersampling) and writes
PNG-compressed ICO and ICNS entries with only the standard library. No
Pillow needed.

The macOS icon puts the logo on a white rounded square, the shape macOS
expects (it frames anything else in a grey plate), with Apple's margins:
an 824 px body on a 1024 px canvas.

The same shapes are drawn by client/src/Logo.cpp and client/web/player.html;
keep them in sync if you change the design.

usage: python make_icon.py   (writes next to this file)
"""
import os
import struct
import zlib

# 64x64 design grid. Back plane first, front plane drawn over it.
BACK = [(32, 26), (58, 40), (32, 54), (6, 40)]
FRONT = [(32, 10), (58, 24), (32, 38), (6, 24)]
BACK_RGB = (0xC7, 0xD2, 0xFE)   # indigo-200
FRONT_RGB = (0x4F, 0x46, 0xE5)  # indigo-600
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SS = 4  # supersamples per axis

# macOS icon, on the same 64-unit grid: body inset by 100/1024 of the canvas,
# corner radius about 22% of the body, logo scaled into the middle.
MAC_INSET = 64 * 100 / 1024
MAC_RADIUS = (64 - 2 * MAC_INSET) * 0.2237
MAC_LOGO_SCALE = 0.62
MAC_BG_RGB = (0xFF, 0xFF, 0xFF)
# (ICNS type, pixel size) for every slot macOS looks up, 1x and 2x.
ICNS_TYPES = [(b"icp4", 16), (b"ic11", 32), (b"icp5", 32), (b"ic12", 64), (b"ic07", 128),
              (b"ic13", 256), (b"ic08", 256), (b"ic14", 512), (b"ic09", 512), (b"ic10", 1024)]


def inside(poly, x, y):
    # Convex polygon, any winding: the point is on the same side of every edge.
    sign = 0
    for i in range(len(poly)):
        (x1, y1), (x2, y2) = poly[i], poly[(i + 1) % len(poly)]
        c = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1)
        if c != 0:
            s = 1 if c > 0 else -1
            if sign and s != sign:
                return False
            sign = s
    return True


def render(size):
    scale = 64 / size
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            back = front = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = (px + (sx + 0.5) / SS) * scale
                    y = (py + (sy + 0.5) / SS) * scale
                    if inside(FRONT, x, y):
                        front += 1
                    elif inside(BACK, x, y):
                        back += 1
            n = SS * SS
            a = (back + front) / n
            if a == 0:
                row += b"\0\0\0\0"
                continue
            # Colour = coverage-weighted mix of the two planes.
            rgb = [round((BACK_RGB[i] * back + FRONT_RGB[i] * front) / (back + front)) for i in range(3)]
            row += bytes(rgb + [round(a * 255)])
        rows.append(bytes(row))
    return rows


def in_rounded_square(x, y):
    lo, hi, r = MAC_INSET, 64 - MAC_INSET, MAC_RADIUS
    if not (lo <= x <= hi and lo <= y <= hi):
        return False
    cx = min(max(x, lo + r), hi - r)
    cy = min(max(y, lo + r), hi - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def render_mac(size):
    k = MAC_LOGO_SCALE
    front = [(32 + (x - 32) * k, 32 + (y - 32) * k) for x, y in FRONT]
    back = [(32 + (x - 32) * k, 32 + (y - 32) * k) for x, y in BACK]
    ss = SS if size <= 256 else 2  # plenty at that resolution, and much faster
    scale = 64 / size
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            acc = [0, 0, 0]
            covered = 0
            for sy in range(ss):
                for sx in range(ss):
                    x = (px + (sx + 0.5) / ss) * scale
                    y = (py + (sy + 0.5) / ss) * scale
                    if not in_rounded_square(x, y):
                        continue
                    covered += 1
                    rgb = FRONT_RGB if inside(front, x, y) else BACK_RGB if inside(back, x, y) else MAC_BG_RGB
                    for i in range(3):
                        acc[i] += rgb[i]
            if covered == 0:
                row += b"\0\0\0\0"
                continue
            row += bytes([round(c / covered) for c in acc] + [round(covered / (ss * ss) * 255)])
        rows.append(bytes(row))
    return rows


def icns(pngs):
    # Each entry: 4-byte type, 4-byte length (header included), PNG data.
    body = b"".join(t + struct.pack(">I", 8 + len(data)) + data for t, data in pngs)
    return b"icns" + struct.pack(">I", 8 + len(body)) + body


def png(size, rows):
    raw = b"".join(b"\0" + r for r in rows)  # filter type 0 per scanline

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def ico(images):
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b"", b""
    for size, data in images:
        dim = 0 if size >= 256 else size  # 0 means 256 in ICO directories
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    return header + entries + blobs


def svg():
    pts = lambda p: " ".join(f"{x},{y}" for x, y in p)
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">'
        f'<polygon points="{pts(BACK)}" fill="#C7D2FE"/>'
        f'<polygon points="{pts(FRONT)}" fill="#4F46E5"/>'
        "</svg>\n"
    )


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    images = [(s, png(s, render(s))) for s in SIZES]
    with open(os.path.join(here, "planar.ico"), "wb") as f:
        f.write(ico(images))
    with open(os.path.join(here, "planar.svg"), "w") as f:
        f.write(svg())
    with open(os.path.join(here, "planar-256.png"), "wb") as f:
        f.write(dict(images)[256])
    mac = {s: png(s, render_mac(s)) for s in sorted({s for _, s in ICNS_TYPES})}
    with open(os.path.join(here, "planar.icns"), "wb") as f:
        f.write(icns([(t, mac[s]) for t, s in ICNS_TYPES]))
    print("wrote planar.ico, planar.icns, planar.svg, planar-256.png")
