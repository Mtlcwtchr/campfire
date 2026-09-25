"""PPM -> PNG, with no dependencies. sim_runner writes the world map as a PPM
because that needs no library; this makes it something a browser will open.

    python3 tools/ppm_to_png.py world.ppm world.png [--shrink N]

--shrink averages N x N cells into one pixel, which is how a two-thousand-cell
world map becomes a picture small enough to keep in the repository.
"""
import sys
import zlib
import struct


def read_ppm(path):
    data = open(path, 'rb').read()
    if data[:2] != b'P6':
        raise SystemExit('not a P6 ppm')
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            while data[pos:pos + 1] not in (b'\n', b''):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    width, height, _ = fields
    return width, height, data[pos:pos + width * height * 3]


def write_png(path, width, height, rgb):
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(kind, payload):
        return (struct.pack('>I', len(payload)) + kind + payload +
                struct.pack('>I', zlib.crc32(kind + payload) & 0xFFFFFFFF))

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def shrink(width, height, rgb, factor):
    """Box-average factor x factor cells into one pixel. Averaged rather than
    sampled: a river is one cell wide, and sampling drops it out of the picture
    entirely."""
    if factor <= 1:
        return width, height, rgb
    out_w, out_h = width // factor, height // factor
    out = bytearray(out_w * out_h * 3)
    for y in range(out_h):
        for x in range(out_w):
            r = g = b = 0
            for dy in range(factor):
                row = (y * factor + dy) * width
                for dx in range(factor):
                    i = (row + x * factor + dx) * 3
                    r += rgb[i]
                    g += rgb[i + 1]
                    b += rgb[i + 2]
            n = factor * factor
            o = (y * out_w + x) * 3
            out[o], out[o + 1], out[o + 2] = r // n, g // n, b // n
    return out_w, out_h, bytes(out)


w, h, rgb = read_ppm(sys.argv[1])
factor = 1
if '--shrink' in sys.argv:
    factor = int(sys.argv[sys.argv.index('--shrink') + 1])
w, h, rgb = shrink(w, h, rgb, factor)
write_png(sys.argv[2], w, h, rgb)
print(f'{sys.argv[2]}: {w}x{h}')

