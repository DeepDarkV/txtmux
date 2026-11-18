#!/usr/bin/env python3
# 生成简单的 launcher 图标（不依赖 PIL：手写 PNG）
import os, struct, zlib

BG = (21, 101, 192)
FG = (255, 255, 255)


def rounded(x, y, w, h, r):
    if x < 0 or y < 0 or x >= w or y >= h:
        return False
    cx = r if x < r else (w - 1 - r if x > w - 1 - r else x)
    cy = r if y < r else (h - 1 - r if y > h - 1 - r else y)
    if (x < r or x > w - 1 - r) and (y < r or y > h - 1 - r):
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return True


def draw(size):
    px = [[(0, 0, 0, 0)] * size for _ in range(size)]
    r = int(size * 0.22)
    for y in range(size):
        for x in range(size):
            if rounded(x, y, size, size, r):
                px[y][x] = BG + (255,)

    def bar(x0, x1, y0, y1):
        for y in range(y0, y1):
            for x in range(x0, x1):
                if 0 <= x < size and 0 <= y < size:
                    px[y][x] = FG + (255,)

    # 上箭头（向右）
    th = max(2, int(size * 0.075))
    y0 = int(size * 0.30)
    bar(int(size * 0.20), int(size * 0.66), y0, y0 + th)
    head = int(size * 0.16)
    for i in range(head):
        h = max(1, int(th * (head - i) / head * 1.6))
        cy = y0 + th // 2
        bar(int(size * 0.62) + i, int(size * 0.62) + i + 1, cy - h // 2, cy + h // 2 + 1)
    # 下箭头（向左）
    y1 = int(size * 0.58)
    bar(int(size * 0.34), int(size * 0.80), y1, y1 + th)
    for i in range(head):
        h = max(1, int(th * (head - i) / head * 1.6))
        cy = y1 + th // 2
        bar(int(size * 0.34) - i - 1, int(size * 0.34) - i, cy - h // 2, cy + h // 2 + 1)
    return px


def write_png(path, px):
    size = len(px)
    raw = b''.join(b'\x00' + b''.join(bytes(p) for p in row) for row in px)

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'res')
for dpi, size in [('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)]:
    d = os.path.join(root, 'mipmap-' + dpi)
    os.makedirs(d, exist_ok=True)
    write_png(os.path.join(d, 'ic_launcher.png'), draw(size))
    print('wrote', os.path.join(d, 'ic_launcher.png'))
