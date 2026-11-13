"""PPM(P6) -> PNG，手写最小编码，不依赖 PIL。
用法：python ppm2png.py a.ppm [b.ppm ...]
"""
import os
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        assert int(f.readline()) == 255
        return w, h, f.read()


def write_png(path, w, h, pixels, channels=3):
    stride = w * channels
    raw = b"".join(b"\x00" + pixels[y * stride:(y + 1) * stride] for y in range(h))
    color_type = 0 if channels == 1 else 2

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color_type, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


for src in sys.argv[1:]:
    if not os.path.exists(src):
        print("missing", src)
        continue
    w, h, px = read_ppm(src)
    out = os.path.splitext(src)[0] + ".png"
    write_png(out, w, h, px)
    print(os.path.basename(out), (w, h), "ok")
