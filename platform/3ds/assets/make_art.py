"""Generate the placeholder HOME Menu art: icons, banners and the banner sound.

Usage: python make_art.py [outdir]     (default: the directory this script is in)

Writes, for each game (undertale, deltarune) and hardware target (n3ds, o3ds):
  icon-<game>-<target>.png    48x48, used for the .3dsx SMDH and the .cia icon
  banner-<game>-<target>.png  256x128, the .cia HOME Menu banner
and one shared banner.wav (a short synthesized two-note chime).

Everything is drawn from code: a plain triangle, flat colours and a hand-made 5x7 pixel font. No game assets, logos or
third-party fonts are used, so the output is ours to commit. Needs Pillow and numpy. The output is deterministic, so
re-running it on an unchanged script leaves the PNGs and WAV byte-identical.
"""
import os
import sys
import wave
import zlib

import numpy as np
from PIL import Image, ImageDraw

# ===[ 5x7 pixel font ]===
# Only the characters the art needs. '#' is ink. g and y have an eighth row for their descender.
GLYPHS = {
    'A': '.###. #...# #...# ##### #...# #...# #...#',
    'B': '####. #...# #...# ####. #...# #...# ####.',
    'D': '####. #...# #...# #...# #...# #...# ####.',
    'E': '##### #.... #.... ####. #.... #.... #####',
    'L': '#.... #.... #.... #.... #.... #.... #####',
    'N': '#...# #...# ##..# #.#.# #..## #...# #...#',
    'O': '.###. #...# #...# #...# #...# #...# .###.',
    'R': '####. #...# #...# ####. #.#.. #..#. #...#',
    'S': '.#### #.... #.... .###. ....# ....# ####.',
    'T': '##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#..',
    'U': '#...# #...# #...# #...# #...# #...# .###.',
    '2': '.###. #...# ....# ...#. ..#.. .#... #####',
    '3': '####. ....# ....# .###. ....# ....# ####.',
    'a': '..... ..... .###. ....# .#### #...# .####',
    'b': '#.... #.... ####. #...# #...# #...# ####.',
    'c': '..... ..... .#### #.... #.... #.... .####',
    'd': '....# ....# .#### #...# #...# #...# .####',
    'e': '..... ..... .###. #...# ##### #.... .###.',
    'f': '..##. .#... ####. .#... .#... .#... .#...',
    'g': '..... ..... .#### #...# #...# .#### ....# .###.',
    'i': '..#.. ..... .##.. ..#.. ..#.. ..#.. .###.',
    'l': '.##.. ..#.. ..#.. ..#.. ..#.. ..#.. .###.',
    'm': '..... ..... ##.#. #.#.# #.#.# #.#.# #...#',
    'n': '..... ..... ####. #...# #...# #...# #...#',
    'o': '..... ..... .###. #...# #...# #...# .###.',
    'r': '..... ..... #.##. ##..# #.... #.... #....',
    's': '..... ..... .#### #.... .###. ....# ####.',
    't': '.#... .#... ####. .#... .#... .#..# ..##.',
    'u': '..... ..... #...# #...# #...# #..## .##.#',
    'w': '..... ..... #...# #...# #.#.# #.#.# .#.#.',
    'y': '..... ..... #...# #...# #...# .#### ....# .###.',
    '-': '..... ..... ..... .###. ..... ..... .....',
    '/': '....# ....# ...#. ..#.. .#... #.... #....',
    ' ': '..... ..... ..... ..... ..... ..... .....',
}
GLYPH_W = 5


def text_width(text, scale=1, spacing=1):
    n = len(text)
    return n * GLYPH_W * scale + (n - 1) * spacing * scale


def draw_text(img, x, y, text, color, scale=1, spacing=1):
    px = img.load()
    for ch in text:
        rows = GLYPHS[ch].split()
        for gy, row in enumerate(rows):
            for gx, cell in enumerate(row):
                if cell != '#':
                    continue
                for sy in range(scale):
                    for sx in range(scale):
                        px[x + gx * scale + sx, y + gy * scale + sy] = color
        x += (GLYPH_W + spacing) * scale


def draw_text_centered(img, cx, y, text, color, scale=1, spacing=1):
    w = text_width(text, scale, spacing)
    draw_text(img, cx - w // 2, y, text, color, scale, spacing)


# ===[ Shapes ]===

def vertical_gradient(w, h, top, bottom):
    t = np.linspace(0.0, 1.0, h)[:, None, None]
    rgb = (1 - t) * np.array(top, float) + t * np.array(bottom, float)
    rgb = np.repeat(rgb, w, axis=1)
    alpha = np.full((h, w, 1), 255.0)
    return Image.fromarray(np.concatenate([rgb, alpha], axis=2).round().astype(np.uint8), 'RGBA').copy()


def triangle(draw, apex_x, top, half_width, height, color, inner, thickness):
    """An upward-pointing outlined triangle (a plain Greek delta)."""
    v = np.array([(apex_x, top), (apex_x + half_width, top + height), (apex_x - half_width, top + height)], float)
    draw.polygon([tuple(p) for p in v], fill=color)
    # Inset every edge by `thickness`: scale the triangle about its incentre by (r - thickness) / r.
    side = np.array([np.linalg.norm(v[1] - v[2]), np.linalg.norm(v[2] - v[0]), np.linalg.norm(v[0] - v[1])])
    incentre = (side[:, None] * v).sum(axis=0) / side.sum()
    (ax, ay), (bx, by) = v[1] - v[0], v[2] - v[0]
    area = abs(ax * by - ay * bx) / 2
    r = 2 * area / side.sum()
    inset = incentre + (v - incentre) * (r - thickness) / r
    draw.polygon([tuple(p) for p in inset], fill=inner)


def stars(img, rect, count, color, seed):
    """Sprinkle dim single-pixel dots in rect (deterministic for a given seed string)."""
    rng = np.random.default_rng(zlib.crc32(seed.encode()))
    x0, y0, x1, y1 = rect
    px = img.load()
    for _ in range(count):
        x = int(rng.integers(x0, x1))
        y = int(rng.integers(y0, y1))
        a = int(rng.integers(60, 160))
        base = px[x, y]
        px[x, y] = tuple(int(base[i] + (color[i] - base[i]) * a / 255) for i in range(3)) + (base[3],)


# ===[ Variants ]===

GAMES = {
    # name: (title, monogram, accent, gradient top, gradient bottom)
    'undertale': ('UNDERTALE', 'UT', (255, 196, 64), (44, 22, 64), (10, 6, 18)),
    'deltarune': ('DELTARUNE', 'DR', (80, 208, 255), (14, 34, 72), (4, 8, 20)),
}
TARGETS = {
    # name: (icon badge, banner line)
    'n3ds': ('N3DS', 'New 3DS build'),
    'o3ds': ('O3DS', 'Old 3DS / 2DS build'),
}
WHITE = (255, 255, 255, 255)


def make_icon(game, target):
    title, monogram, accent, top, bottom = GAMES[game]
    badge, _ = TARGETS[target]
    img = vertical_gradient(48, 48, top, bottom)
    stars(img, (2, 2, 46, 24), 10, (255, 255, 255), seed=f'{game}-icon')
    draw = ImageDraw.Draw(img)
    triangle(draw, 24, 3, 15, 19, accent + (255,), top + (255,), 2)
    draw_text_centered(img, 24, 25, monogram, WHITE, scale=2)
    # Hardware badge on a darker strip so N3DS and O3DS builds are told apart on the HOME Menu.
    draw.rectangle([0, 40, 47, 47], fill=tuple(c // 2 for c in bottom) + (255,))
    draw_text_centered(img, 24, 41, badge, accent + (255,))
    return img


def make_banner(game, target):
    title, monogram, accent, top, bottom = GAMES[game]
    _, line = TARGETS[target]
    w, h = 256, 128
    panel = vertical_gradient(w, h, top, bottom)
    stars(panel, (12, 16, 244, 112), 70, (255, 255, 255), seed=f'{game}-banner')
    # Rounded panel with transparent margins; the HOME Menu shows the banner on a 3D card.
    mask = Image.new('L', (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([8, 12, 247, 115], radius=12, fill=255)
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    img.paste(panel, (0, 0), mask)
    draw = ImageDraw.Draw(img)
    draw.rounded_rectangle([8, 12, 247, 115], radius=12, outline=accent + (255,), width=2)

    triangle(draw, 44, 30, 26, 46, accent + (255,), top + (255,), 4)
    draw_text(img, 79, 34, title, accent + (255,), scale=3)
    draw_text(img, 80, 62, line, WHITE)
    draw_text_centered(img, 128, 100, 'unofficial - uses your own game files', (200, 200, 210, 255))
    return img


def make_chime(path, rate=32000):
    """Two soft sine notes (E5 then B5) with a fast attack and exponential decay, about 1.2 s."""
    t_total = 1.2
    n = int(rate * t_total)
    out = np.zeros(n)
    for start, freq in ((0.0, 659.25), (0.18, 987.77)):
        s = int(start * rate)
        t = np.arange(n - s) / rate
        env = np.minimum(t / 0.008, 1.0) * np.exp(-t * 4.0)
        tone = np.sin(2 * np.pi * freq * t) + 0.25 * np.sin(2 * np.pi * 2 * freq * t)
        out[s:] += env * tone
    fade = np.ones(n)
    fade[-int(0.05 * rate):] = np.linspace(1.0, 0.0, int(0.05 * rate))
    out *= fade
    out *= 0.5 / np.max(np.abs(out))  # peak at -6 dBFS
    pcm = (out * 32767).round().astype('<i2')
    with wave.open(path, 'wb') as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(rate)
        f.writeframes(pcm.tobytes())


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    os.makedirs(outdir, exist_ok=True)
    for game in GAMES:
        for target in TARGETS:
            make_icon(game, target).save(os.path.join(outdir, f'icon-{game}-{target}.png'), optimize=True)
            make_banner(game, target).save(os.path.join(outdir, f'banner-{game}-{target}.png'), optimize=True)
    make_chime(os.path.join(outdir, 'banner.wav'))
    print(f'wrote art to {outdir}')


if __name__ == '__main__':
    main()
