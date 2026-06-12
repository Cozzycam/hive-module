"""Extract a named RGB565 sprite from firmware/include/sprites.h to a PNG
with alpha (0xF81F colorkey -> transparent).
Usage: py tools/extract_sprite.py QUEEN app/public/queen.png
"""
import re
import sys

from PIL import Image

TRANSPARENT = 0xF81F

def main():
    name, out_path = sys.argv[1], sys.argv[2]
    src = open("firmware/include/sprites.h", encoding="utf-8").read()

    w = int(re.search(rf"#define {name}_W (\d+)", src).group(1))
    h = int(re.search(rf"#define {name}_H (\d+)", src).group(1))
    body = re.search(rf"{name}\[\d+\] PROGMEM = \{{(.*?)\}};", src, re.S).group(1)
    vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{1,4})", body)]
    assert len(vals) == w * h, f"expected {w*h} pixels, found {len(vals)}"

    img = Image.new("RGBA", (w, h))
    px = img.load()
    for i, c in enumerate(vals):
        x, y = i % w, i // w
        if c == TRANSPARENT:
            px[x, y] = (0, 0, 0, 0)
        else:
            r = ((c >> 11) & 0x1F) * 255 // 31
            g = ((c >> 5) & 0x3F) * 255 // 63
            b = (c & 0x1F) * 255 // 31
            px[x, y] = (r, g, b, 255)
    img.save(out_path)
    print(f"{name} {w}x{h} -> {out_path}")

if __name__ == "__main__":
    main()
