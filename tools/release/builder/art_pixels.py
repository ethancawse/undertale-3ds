"""Small, deterministic RGBA operations for personal HOME Menu art. No image resampling libraries."""
from __future__ import annotations

import dataclasses
import hashlib
import struct
import zlib


@dataclasses.dataclass(frozen=True)
class Image:
    width: int
    height: int
    rgba: bytes

    def __post_init__(self):
        if not (0 < self.width <= 4096 and 0 < self.height <= 4096
                and len(self.rgba) == self.width * self.height * 4):
            raise ValueError("Invalid art image size")

    def pixel(self, x, y):
        p = (y * self.width + x) * 4
        return self.rgba[p:p + 4]

    def crop(self, x, y, width, height):
        if x < 0 or y < 0 or x + width > self.width or y + height > self.height:
            raise ValueError("Art crop exceeds texture")
        return Image(width, height, b"".join(self.rgba[((y + r) * self.width + x) * 4:
                                                     ((y + r) * self.width + x + width) * 4]
                                             for r in range(height)))

    def digest(self):
        return hashlib.sha256(struct.pack("<II", self.width, self.height) + self.rgba).hexdigest()


def blank(width, height, colour=(0, 0, 0, 255)):
    return Image(width, height, bytes(colour) * (width * height))


def nearest(image, scale):
    if type(scale) is not int or scale < 1:
        raise ValueError("Art scale must be a positive integer")
    rows = []
    for y in range(image.height):
        row = b"".join(image.pixel(x, y) * scale for x in range(image.width))
        rows.extend([row] * scale)
    return Image(image.width * scale, image.height * scale, b"".join(rows))


def paste(canvas, image, x, y):
    if x < 0 or y < 0 or x + image.width > canvas.width or y + image.height > canvas.height:
        raise ValueError("Art does not fit canvas")
    out = bytearray(canvas.rgba)
    for iy in range(image.height):
        for ix in range(image.width):
            r, g, b, a = image.pixel(ix, iy)
            p = ((y + iy) * canvas.width + x + ix) * 4
            if a == 255:
                out[p:p + 4] = bytes((r, g, b, 255))
            elif a:
                # Integer alpha composition; never filters or mixes adjacent pixels.
                out[p:p + 4] = bytes(tuple((v * a + out[p + c] * (255 - a) + 127) // 255
                                                  for c, v in enumerate((r, g, b))) + (255,))
    return Image(canvas.width, canvas.height, bytes(out))


def centred(image, width, height, scale=None):
    scale = min(width // image.width, height // image.height) if scale is None else scale
    rendered = nearest(image, scale)
    return paste(blank(width, height), rendered, (width - rendered.width) // 2, (height - rendered.height) // 2)


def trim(image):
    points = [(x, y) for y in range(image.height) for x in range(image.width) if image.pixel(x, y)[3]]
    if not points:
        raise ValueError("Empty art")
    x0, y0 = min(x for x, y in points), min(y for x, y in points)
    x1, y1 = max(x for x, y in points), max(y for x, y in points)
    return image.crop(x0, y0, x1 - x0 + 1, y1 - y0 + 1)


def png(image):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    rows = b"".join(b"\0" + image.rgba[y * image.width * 4:(y + 1) * image.width * 4]
                    for y in range(image.height))
    # Stored DEFLATE blocks keep the entire PNG byte-identical across Python/zlib versions and platforms.
    # These private 256x128/48x48 inputs are tiny; the final SMDH/banner tools encode their own textures.
    compressed = bytearray(b"\x78\x01")
    for start in range(0, len(rows), 65535):
        block = rows[start:start + 65535]
        compressed.extend(bytes((int(start + len(block) == len(rows)),)))
        compressed.extend(struct.pack("<HH", len(block), len(block) ^ 0xffff))
        compressed.extend(block)
    compressed.extend(struct.pack(">I", zlib.adler32(rows)))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", image.width, image.height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", bytes(compressed)) + chunk(b"IEND", b""))


def read_png(data):
    """8-bit RGB/RGBA texture pages. Bounds and CRCs checked before decompression."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Not a PNG texture")
    pos, compressed = 8, bytearray()
    width = height = channels = 0
    while pos + 12 <= len(data):
        size, tag = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + size]
        if pos + size + 12 > len(data) or zlib.crc32(tag + body) != struct.unpack_from(">I", data, pos + 8 + size)[0]:
            raise ValueError("Invalid PNG chunk")
        if tag == b"IHDR":
            width, height, bits, colour, comp, filt, interlace = struct.unpack(">IIBBBBB", body)
            if bits != 8 or colour not in (2, 6) or comp or filt or interlace:
                raise ValueError("Unsupported PNG texture")
            channels = 4 if colour == 6 else 3
        elif tag == b"IDAT":
            compressed.extend(body)
        elif tag == b"IEND":
            break
        pos += size + 12
    if not (0 < width <= 4096 and 0 < height <= 4096 and channels):
        raise ValueError("Invalid PNG size")
    stride = width * channels
    expected = height * (stride + 1)
    raw = zlib.decompressobj().decompress(compressed, expected + 1)
    if len(raw) != expected:
        raise ValueError("Truncated PNG texture")
    previous, out = bytearray(stride), bytearray()
    for y in range(height):
        start = y * (stride + 1)
        mode = raw[start]
        if mode > 4:
            raise ValueError("Invalid PNG filter")
        row = bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up, corner = previous[x], previous[x - channels] if x >= channels else 0
            if mode == 1:
                predictor = left
            elif mode == 2:
                predictor = up
            elif mode == 3:
                predictor = (left + up) // 2
            elif mode == 4:
                p = left + up - corner
                distances = (abs(p - left), abs(p - up), abs(p - corner))
                predictor = (left, up, corner)[distances.index(min(distances))]
            else:
                predictor = 0
            row[x] = (row[x] + predictor) & 255
        if channels == 4:
            out.extend(row)
        else:
            for x in range(width):
                out.extend(row[x * 3:x * 3 + 3] + b"\xff")
        previous = row
    return Image(width, height, bytes(out))


def read_qoi(data):
    """GameMaker's fioq variant, as used by engine/src/qoi.c and tools/analysis/texdouble.py."""
    if data[:4] != b"fioq":
        raise ValueError("Not a GameMaker QOI texture")
    width, height, size = struct.unpack_from("<HHI", data, 4)
    if not (0 < width <= 4096 and 0 < height <= 4096 and size <= len(data) - 12):
        raise ValueError("Invalid QOI size")
    stream, pos, run = data[12:12 + size], 0, 0
    index = [(0, 0, 0, 0)] * 64
    r = g = b = 0
    a = 255
    out = bytearray()

    def signed(value, bits):
        return value - (1 << bits) if value & (1 << (bits - 1)) else value

    for _ in range(width * height):
        if run:
            run -= 1
        else:
            op = stream[pos]; pos += 1
            if op & 0xc0 == 0:
                r, g, b, a = index[op]
            elif op & 0xe0 == 0x40:
                run = op & 31
            elif op & 0xe0 == 0x60:
                run = ((op & 31) << 8 | stream[pos]) + 32; pos += 1
            elif op & 0xc0 == 0x80:
                r, g, b = ((r + signed(op >> 4 & 3, 2)) & 255,
                           (g + signed(op >> 2 & 3, 2)) & 255, (b + signed(op & 3, 2)) & 255)
            elif op & 0xe0 == 0xc0:
                value = op << 8 | stream[pos]; pos += 1
                r, g, b = ((r + signed(value >> 8 & 31, 5)) & 255,
                           (g + signed(value >> 4 & 15, 4)) & 255, (b + signed(value & 15, 4)) & 255)
            elif op & 0xf0 == 0xe0:
                value = op << 16 | stream[pos] << 8 | stream[pos + 1]; pos += 2
                r, g, b, a = ((r + signed(value >> 15 & 31, 5)) & 255,
                              (g + signed(value >> 10 & 31, 5)) & 255,
                              (b + signed(value >> 5 & 31, 5)) & 255, (a + signed(value & 31, 5)) & 255)
            else:
                if op & 8: r = stream[pos]; pos += 1
                if op & 4: g = stream[pos]; pos += 1
                if op & 2: b = stream[pos]; pos += 1
                if op & 1: a = stream[pos]; pos += 1
            index[(r ^ g ^ b ^ a) & 63] = (r, g, b, a)
        out.extend((r, g, b, a))
    return Image(width, height, bytes(out))
