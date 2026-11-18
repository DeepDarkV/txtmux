#!/usr/bin/env python3
"""Generate txtmux.ico (16/32/48) without any image library.

Design: rounded blue tile + white "T" glyph (txt) + green dot (active).
Pure-python ICO writer: 32bpp BGRA BMP images + 1bpp AND mask.
"""
import struct
import sys

SIZES = (16, 32, 48)
BG = (0xE8, 0x8A, 0x1E, 0xFF)      # BGRA: blue #1E8AE8
FG = (0xFF, 0xFF, 0xFF, 0xFF)      # white
DOT = (0x4A, 0xD1, 0x34, 0xFF)     # green #34D14A
CLEAR = (0, 0, 0, 0)


def rounded_mask(size):
    """True inside a rounded square (radius ~ 18% of size)."""
    r = max(2, int(round(size * 0.18)))
    pts = []
    for y in range(size):
        row = []
        for x in range(size):
            # corner circles
            cx = cy = None
            if x < r and y < r:
                cx, cy = r - 0.5, r - 0.5
            elif x >= size - r and y < r:
                cx, cy = size - r - 0.5, r - 0.5
            elif x < r and y >= size - r:
                cx, cy = r - 0.5, size - r - 0.5
            elif x >= size - r and y >= size - r:
                cx, cy = size - r - 0.5, size - r - 0.5
            if cx is None:
                row.append(True)
            else:
                dx, dy = x - cx, y - cy
                row.append(dx * dx + dy * dy <= r * r)
        pts.append(row)
    return pts


def image(size):
    mask = rounded_mask(size)
    px = [[CLEAR for _ in range(size)] for _ in range(size)]
    for y in range(size):
        for x in range(size):
            if mask[y][x]:
                px[y][x] = BG

    def fill(x0, y0, x1, y1, color):
        for y in range(max(0, y0), min(size, y1)):
            for x in range(max(0, x0), min(size, x1)):
                if mask[y][x]:
                    px[y][x] = color

    # "T": top bar + stem, centred
    tw = int(round(size * 0.62))
    th = max(2, int(round(size * 0.13)))
    tx = (size - tw) // 2
    ty = int(round(size * 0.22))
    fill(tx, ty, tx + tw, ty + th, FG)                     # bar
    sw = max(2, int(round(size * 0.14)))
    fill((size - sw) // 2, ty, (size - sw) // 2 + sw, int(round(size * 0.78)), FG)

    # green status dot bottom-right
    d = max(3, int(round(size * 0.26)))
    dx0, dy0 = size - d - max(1, int(round(size * 0.06))), size - d - max(1, int(round(size * 0.06)))
    cr = (d - 1) / 2.0
    ccx, ccy = dx0 + cr, dy0 + cr
    for y in range(max(0, dy0 - 1), min(size, dy0 + d + 1)):
        for x in range(max(0, dx0 - 1), min(size, dx0 + d + 1)):
            if not mask[y][x]:
                continue
            if (x - ccx) ** 2 + (y - ccy) ** 2 <= cr * cr:
                px[y][x] = DOT
    return px


def bmp_entry(size):
    px = image(size)
    hdr = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    body = bytearray()
    for y in range(size - 1, -1, -1):          # bottom-up
        for x in range(size):
            b, g, r, a = px[y][x]
            body += bytes((b, g, r, a))
    # AND mask: 1bpp, rows padded to 4 bytes; 0 = opaque
    rowbytes = ((size + 31) // 32) * 4
    mask = bytearray()
    for y in range(size - 1, -1, -1):
        bits = bytearray(rowbytes)
        for x in range(size):
            if px[y][x][3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return hdr + bytes(body) + bytes(mask)


def main(out):
    imgs = [bmp_entry(s) for s in SIZES]
    off = 6 + 16 * len(imgs)
    entries = b''
    data = []
    for s, blob in zip(SIZES, imgs):
        entries += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(blob), off)
        off += len(blob)
        data.append(blob)
    with open(out, 'wb') as f:
        f.write(struct.pack('<HHH', 0, 1, len(imgs)) + entries + b''.join(data))
    print('wrote', out, sum(len(b) for b in imgs) + 6 + 16 * len(imgs), 'bytes')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'txtmux.ico')
