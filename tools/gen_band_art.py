# tools/gen_band_art.py -- Generate the green-phosphor A8 home backdrop (240x80, two levels: 0/255).
#
# High-contrast poster style: silhouettes pure black, lights/moon pure bright, only sky/halo use Bayer ordered dots --
# so the subject is recognizable at a glance on a tiny image.
# Shown on device via lv_image + image_recolor (green); built-in image lives in flash (.rodata), zero RAM.
#
# Usage: python tools/gen_band_art.py   (run from the repo root)
# Output: main/apps/home/band_art.c / band_art.h

import math
import random
from PIL import Image, ImageDraw

W, H = 240, 80
OUT_C = "main/apps/home/band_art.c"
OUT_H = "main/apps/home/band_art.h"

# 4x4 Bayer matrix (0..15): gray > threshold -> lit dot. Regular dots look more like a phosphor screen than Floyd noise.
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def bayer_dither(img):
    px = img.load()
    out = Image.new("L", (W, H), 0)
    po = out.load()
    for y in range(H):
        for x in range(W):
            t = (BAYER[y % 4][x % 4] + 0.5) * 16
            po[x, y] = 255 if px[x, y] > t else 0
    return out


def vgrad(draw, top, bottom):
    for y in range(H):
        v = top + (bottom - top) * y // H
        draw.line([(0, y), (W - 1, y)], fill=v)


def glow(draw, cx, cy, peak, spread):
    """Moon radial halo: peak at center, linear falloff to 0."""
    for y in range(max(0, cy - spread), min(H, cy + spread)):
        for x in range(max(0, cx - spread), min(W, cx + spread)):
            d = math.hypot(x - cx, y - cy)
            if d > spread:
                continue
            v = int(peak * max(0.0, 1.0 - d / spread))
            if v:
                draw.point((x, y), fill=max(v, 0))


def stars(draw, n, ymax, seed):
    rnd = random.Random(seed)
    for _ in range(n):
        x, y = rnd.randrange(W), rnd.randrange(ymax)
        draw.point((x, y), fill=255)
        if rnd.random() < 0.3:   # cross-shaped star
            draw.point((x - 1, y), fill=120)
            draw.point((x + 1, y), fill=120)


def draw_city():
    """Scene 1: river city night -- moon + stars + TV tower + river bridge + buildings + water reflections."""
    horizon = 54
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)

    vgrad(d, 18, 52)
    stars(d, 20, 44, seed=20260913)
    glow(d, 196, 15, 150, 16)
    d.ellipse([196 - 8, 15 - 8, 196 + 8, 15 + 8], fill=255)
    d.ellipse([196 - 2, 15 - 5, 196 + 4, 15 + 2], fill=170)   # moon dark patch

    # ---- Landmark TV tower (tapered shaft + observation pod + antenna) ----
    for i, y in enumerate(range(16, horizon)):
        t = (y - 16) / (horizon - 16)
        half = 2 + int(6 * t)                     # 2 → 8
        d.line([(38 - half, y), (38 + half, y)], fill=6)
        if i % 6 == 3:                            # shaft light ring
            d.line([(38 - half - 1, y), (38 - half, y)], fill=255)
            d.line([(38 + half, y), (38 + half + 1, y)], fill=255)
    d.rectangle([32, 32, 45, 39], fill=6)          # observation pod
    d.rectangle([32, 32, 45, 34], fill=255)        # pod light strip
    d.line([(38, 16), (38, 7)], fill=6)            # antenna
    d.point((38, 6), fill=255)                     # antenna top light

    # ---- River bridge (deck + twin towers + cables + bridge lights) ----
    deck_y = horizon - 9
    d.line([(148, deck_y), (238, deck_y)], fill=2, width=2)
    for tx in (168, 218):                          # bridge towers
        d.line([(tx, deck_y - 16), (tx, deck_y + 3)], fill=2, width=2)
        d.point((tx, deck_y - 17), fill=255)       # tower top light
    for i in range(9):                             # cables
        x = 152 + i * 10
        top = 168 if x < 193 else 218
        d.line([(x, deck_y), (top, deck_y - 14)], fill=30)
    for lx in range(150, 238, 6):                  # deck street lights
        d.point((lx, deck_y - 2), fill=255)

    # ---- Distant buildings (mid gray -> sparse dots, like silhouettes in fog) ----
    rnd = random.Random(7)
    x = 56
    while x < 148:
        bw = rnd.randrange(12, 22)
        bh = rnd.randrange(8, 18)
        d.rectangle([x, horizon - bh, x + bw, horizon], fill=55)
        x += bw + rnd.randrange(2, 5)

    # ---- Near buildings (pure black silhouettes + lit windows; right side left clear for the bridge) ----
    x = 50
    while x < 148:
        bw = rnd.randrange(16, 30)
        bh = rnd.randrange(14, 36)
        top = horizon - bh
        d.rectangle([x, top, x + bw, horizon], fill=2)
        for wy in range(top + 3, horizon - 3, 5):
            for wx in range(x + 2, x + bw - 2, 4):
                if rnd.random() < 0.16:
                    d.rectangle([wx, wy, wx + 1, wy + 1], fill=255)
        if rnd.random() < 0.35:                    # antenna
            ax = x + bw // 2
            d.line([(ax, top), (ax, top - 5)], fill=2)
            d.point((ax, top - 6), fill=255)
        x += bw + rnd.randrange(2, 6)

    # ---- Water: dark base + distinct light-source reflections ----
    d.rectangle([0, horizon, W - 1, H - 1], fill=5)
    for y in range(horizon + 2, H, 3):             # moon reflection column
        ln = max(2, 10 - (y - horizon) // 4)
        j = rnd.randrange(-2, 3)
        d.line([(196 - ln // 2 + j, y), (196 + ln // 2 + j, y)], fill=255)
    for lx in range(150, 238, 6):                  # bridge light reflections (short vertical glints)
        if rnd.random() < 0.7:
            y0 = deck_y + 4 + rnd.randrange(0, 4)
            if y0 < H - 2:
                d.line([(lx, y0), (lx, y0 + 2)], fill=170)
    for _ in range(10):                            # scattered ripples
        y = rnd.randrange(horizon + 3, H)
        ln = rnd.randrange(5, 14)
        x0 = rnd.randrange(0, W - ln)
        d.line([(x0, y), (x0 + ln, y)], fill=70)
    return img


def draw_mountain():
    """Scene 2: mountain lake moonlight -- big moon + three ridge layers + lake reflection + pines + small boat with fishing light."""
    horizon = 48
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)

    vgrad(d, 14, 46)
    stars(d, 24, 38, seed=31415)
    glow(d, 64, 17, 160, 18)
    d.ellipse([64 - 9, 17 - 9, 64 + 9, 17 + 9], fill=255)
    d.ellipse([64 - 2, 17 - 6, 64 + 5, 17 + 3], fill=170)

    # Three ridge layers: far (dots) -> mid -> near (pure black), sine-sum outlines
    def ridge(base, amp, freq, phase, fill):
        pts = [(0, H)]
        for x in range(0, W + 1, 3):
            y = base - int(amp * (math.sin(x / freq + phase) * 0.6 +
                                  math.sin(x / (freq * 0.37) + phase * 2.7) * 0.4))
            pts.append((x, y))
        pts.append((W, H))
        d.polygon(pts, fill=fill)

    ridge(horizon - 7, 11, 30.0, 0.7, 44)
    ridge(horizon - 3, 8, 22.0, 2.3, 20)
    ridge(horizon + 2, 6, 16.0, 4.4, 2)

    # Near-shore pines (left and right clumps, pure black stacked triangles)
    def pine(cx, base, h, w):
        for i in range(3):
            top = base - h + i * (h // 3)
            half = int(w * (i + 1) / 3 / 2) + 1
            d.polygon([(cx, top - 2), (cx - half, base - (h // 3) * (2 - i) + 2),
                       (cx + half, base - (h // 3) * (2 - i) + 2)], fill=2)
        d.line([(cx, base), (cx, base + 2)], fill=2)

    for cx, h, w in ((14, 18, 12), (26, 24, 14), (38, 16, 10)):
        pine(cx, horizon + 4, h, w)
    for cx, h, w in ((224, 21, 13), (234, 15, 10)):
        pine(cx, horizon + 3, h, w)

    # Lake: dark base + moon reflection + small boat fishing light + ripples
    rnd = random.Random(4242)
    d.rectangle([0, horizon, W - 1, H - 1], fill=5)
    for y in range(horizon + 2, H, 3):
        ln = max(3, 12 - (y - horizon) // 3)
        j = rnd.randrange(-3, 4)
        d.line([(64 - ln // 2 + j, y), (64 + ln // 2 + j, y)], fill=255)

    bx, by = 152, horizon + 9                      # boat: hull + fishing light
    d.polygon([(bx, by), (bx + 20, by), (bx + 16, by + 4), (bx + 4, by + 4)], fill=34)
    d.line([(bx + 10, by - 6), (bx + 10, by)], fill=2)   # mast
    d.point((bx + 10, by - 7), fill=255)                 # fishing light
    glow(d, bx + 10, by - 7, 90, 4)
    if by + 6 < H:
        d.line([(bx + 8, by + 6), (bx + 13, by + 6)], fill=200)  # lamp reflection on water

    for _ in range(8):
        y = rnd.randrange(horizon + 4, H)
        ln = rnd.randrange(5, 12)
        x0 = rnd.randrange(0, W - ln)
        d.line([(x0, y), (x0 + ln, y)], fill=60)
    return img


def to_a8(img):
    """Gray -> Bayer 1-bit -> A8 bytes (0/255)."""
    bw = bayer_dither(img)
    px = bw.load()
    out = bytearray()
    for y in range(H):
        for x in range(W):
            out.append(255 if px[x, y] else 0)
    return bytes(out)


def emit(name, data, lines):
    hexlines = []
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        hexlines.append("    " + " ".join(f"0x{b:02X}," for b in chunk))
    body = "\n".join(hexlines)
    lines.append(f"// {name}: {len(data)} bytes (240x{H} A8, 0/255 only)")
    lines.append(f"const uint8_t {name}[{len(data)}] = {{\n{body}\n}};\n")


def main():
    scenes = [("kBandArtCity", draw_city()), ("kBandArtMountain", draw_mountain())]

    c = [
        "// main/apps/home/band_art.c -- home green-phosphor backdrop (A8 alpha image, generated by tools/gen_band_art.py).",
        "// Shown via lv_image + style image_recolor (phosphor green): alpha 1 -> green dot, 0 -> black background shows through.",
        "// Built-in image lives in flash, zero RAM; to regenerate run python tools/gen_band_art.py, do not edit by hand.",
        "#include \"apps/home/band_art.h\"",
        "",
    ]
    for name, img in scenes:
        emit(name, to_a8(img), c)

    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(c))

    h = [
        "// main/apps/home/band_art.h -- built-in backdrop (A8). See the header comment in band_art.c.",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "#include \"lvgl.h\"",
        "",
        "#ifdef __cplusplus",
        "extern \"C\" {",
        "#endif",
        "",
        "#define BAND_ART_W 240",
        "#define BAND_ART_H 80",
        "",
        "extern const uint8_t kBandArtCity[BAND_ART_W * BAND_ART_H];",
        "extern const uint8_t kBandArtMountain[BAND_ART_W * BAND_ART_H];",
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
    ]
    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(h) + "\n")

    print("OK: wrote", OUT_C, "and", OUT_H)


if __name__ == "__main__":
    main()
