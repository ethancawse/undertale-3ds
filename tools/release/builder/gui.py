"""The builder's window: one wizard, nine pages, Back and Next (docs/release/builder.md, "Wizard"). Plain tkinter.

UNDERTALE theme:
- Black background (#000000) and white pixel-border boxes (#ffffff).
- Pixel bitmap font:
  (a) Before the game copy is found or if its assets are unreadable: public-domain pixel font and original heart.
  (b) Once the player's copy is found on disk, loads the game's own bitmap font (fnt_maintext) and red SOUL sprite
      (spr_heart) from data.win, game.ios or game.unx at runtime (strictly read-only), falling back to (a).
      Boring mode always uses standard platform widgets and the system font.
- Red SOUL (#ff0000) selection marker on radio/check/button controls and progress marker on progress bars.
- UNDERTALE-style orange (#ff7f27) / yellow (#ffff00) pixel-bordered buttons.
"""
from __future__ import annotations

import os
import json
import queue
import struct
import subprocess
import sys
import threading
import time
import types
import tkinter as tk
import tkinter.font as tkfont
import traceback
import webbrowser
import zlib
from tkinter import filedialog, messagebox, ttk

import deliver
import cleanup
import ftp_delivery
import ftp_saves
import discover
import pipeline
import products
import preferences
import updates
import toolchain
from processes import hidden_options

DISCLAIMER = "Unofficial fan project. Not affiliated with Toby Fox, Fangamer, YoYo Games, or Nintendo."
WHAT = ('Makes a 3DS version of your own PC game and puts it on your 3DS by SD card or Wi-Fi.\n'
        'The first setup and first build need an internet connection.')
TERMS = "Store terms for your copy (Steam, GOG) may restrict modifying or converting the game. Building is your decision."
# Toby Fox's credit first, the port's copyright last (ruled 10-22: the game's author first, the port's name lower).
# One notice inside the main window (App.inform), without headings, so its OK button stays in the window at the
# window's smallest size. {holder} is port_holder(): the port's name is written once, in the approved public credit.
ABOUT = ("UNDERTALE and DELTARUNE are by Toby Fox. All game art, music and text belong to Toby Fox "
         "and are read from your own copy of the game.\n\n"
         "{app} builder\n\n"
         "Free software under the GNU AGPL-3.0. Source ZIP:\n{source}\n\n"
         "The builder checks your game files against known official copies.\n\n" + DISCLAIMER + "\n\n"
         "Port source code: Copyright (C) 2026 {holder} and contributors. AGPL-3.0.")
ABOUT_NO_HOLDER = "this port's authors"


def port_holder():
    """The name under "3DS port" in docs/release/credits-port.txt, as the options menu's Credits page shows it
    (tools/aot/credits.py), or None. That file is the one place the kit's owner-name rule allows the port's name
    (tools/release/check_kit_hygiene.py); the 3DS Legal page takes it from there too (platform/3ds/CMakeLists.txt)."""
    try:
        with open(os.path.join(pipeline.SOURCE_ROOT, 'docs', 'release', 'credits-port.txt'), encoding='utf-8') as f:
            lines = [line.rstrip('\r\n') for line in f]
        line = lines[lines.index('H3DS port') + 1]
    except (OSError, UnicodeError, ValueError, IndexError):
        return None
    name = line[1:].strip()
    return name if line.startswith('N') and name else None
WRAP = 640
SHORT_TARGET = {"n3ds": "New 3DS", "o3ds": "Old 3DS"}


def _fmt_secs(s):
    return "%d min" % max(1, round(s / 60)) if s >= 90 else "%d s" % max(1, round(s))


BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
YELLOW = (255, 255, 0)
ORANGE = (255, 127, 39)
RED = (255, 0, 0)
GRAY = (150, 150, 150)
DARK_GRAY = (85, 85, 85)

# ---------------------------------------------------------------------------------------------------------------------
# Fallback (a): Public Domain 8x8 pixel font (font8x8_basic, Unlicense / Public Domain,
# https://github.com/dhepper/font8x8/blob/master/font8x8_basic.h) for ASCII 0x20..0x7E.
FONT8X8_BASIC = (
    (0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00), (0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00),
    (0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00), (0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00),
    (0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00), (0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00),
    (0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00), (0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00),
    (0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00), (0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00),
    (0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00), (0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00),
    (0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06), (0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00),
    (0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00), (0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00),
    (0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00), (0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00),
    (0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00), (0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00),
    (0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00), (0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00),
    (0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00), (0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00),
    (0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00), (0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00),
    (0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00), (0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06),
    (0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00), (0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00),
    (0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00), (0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00),
    (0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00), (0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00),
    (0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00), (0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00),
    (0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00), (0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00),
    (0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00), (0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00),
    (0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00), (0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00),
    (0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00), (0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00),
    (0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00), (0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00),
    (0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00), (0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00),
    (0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00), (0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00),
    (0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00), (0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00),
    (0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00), (0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00),
    (0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00), (0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00),
    (0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00), (0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00),
    (0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00), (0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00),
    (0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00), (0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00),
    (0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00), (0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF),
    (0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00), (0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00),
    (0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00), (0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00),
    (0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00), (0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00),
    (0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x0F, 0x00), (0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F),
    (0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00), (0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00),
    (0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E), (0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00),
    (0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00), (0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00),
    (0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00), (0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00),
    (0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F), (0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78),
    (0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00), (0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00),
    (0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00), (0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00),
    (0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00), (0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00),
    (0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00), (0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F),
    (0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00), (0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00),
    (0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00), (0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00),
    (0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
)

# Original 16x16 heart drawn for the builder (not taken from any game file), shown until data.win is loaded.
FALLBACK_SOUL_ROWS = (
    "................",
    "..####....####..",
    ".######..######.",
    "################",
    "################",
    "################",
    "################",
    ".##############.",
    "..############..",
    "...##########...",
    "....########....",
    ".....######.....",
    "......####......",
    ".......##.......",
    "................",
    "................",
)


def _decode_png_rgba(png_bytes, crop_y0=0, crop_h=None):
    """Minimal stdlib PNG RGBA decoder (8-bit color types 2, 3, 6) for reading runtime font/SOUL sheets from data.win."""
    if png_bytes[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos = 8
    w = h = bit_depth = color_type = interlace = 0
    plte = b""
    trns = b""
    idat = bytearray()
    while pos + 8 <= len(png_bytes):
        clen, ctype = struct.unpack(">I4s", png_bytes[pos:pos + 8])
        cdata = png_bytes[pos + 8:pos + 8 + clen]
        pos += 12 + clen
        if ctype == b"IHDR":
            w, h, bit_depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", cdata)
            if bit_depth != 8 or interlace != 0 or color_type not in (2, 3, 6):
                return None
        elif ctype == b"PLTE":
            plte = cdata
        elif ctype == b"tRNS":
            trns = cdata
        elif ctype == b"IDAT":
            idat.extend(cdata)
        elif ctype == b"IEND":
            break
    if not w or not h:
        return None
    bpp = 4 if color_type == 6 else (3 if color_type == 2 else 1)
    stride = w * bpp
    max_y = min(h, crop_y0 + crop_h) if crop_h else h
    try:
        raw = zlib.decompressobj().decompress(bytes(idat), (stride + 1) * max_y)
    except zlib.error:
        return None
    if len(raw) < (stride + 1) * max_y:
        return None
    prev = bytearray(stride)
    rows = []
    for y in range(max_y):
        rstart = y * (stride + 1)
        filt = raw[rstart]
        cur = bytearray(raw[rstart + 1:rstart + 1 + stride])
        if filt == 1:
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 0xFF
        elif filt == 2:
            for i in range(stride):
                cur[i] = (cur[i] + prev[i]) & 0xFF
        elif filt == 3:
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif filt == 4:
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 0xFF
        prev = cur
        if y >= crop_y0:
            if color_type == 6:
                rows.append(bytes(cur))
            elif color_type == 2:
                rgba = bytearray(w * 4)
                for x in range(w):
                    rgba[x * 4:x * 4 + 3] = cur[x * 3:x * 3 + 3]
                    rgba[x * 4 + 3] = 255
                rows.append(bytes(rgba))
            elif color_type == 3:
                rgba = bytearray(w * 4)
                for x in range(w):
                    idx = cur[x]
                    rgba[x * 4:x * 4 + 3] = plte[idx * 3:idx * 3 + 3]
                    rgba[x * 4 + 3] = trns[idx] if idx < len(trns) else 255
                rows.append(bytes(rgba))
    return w, rows


def _fallback_glyphs():
    glyphs = {}
    for i, rows8 in enumerate(FONT8X8_BASIC):
        ch = chr(32 + i)
        g_rows = []
        for b in rows8:
            row_bits = tuple(bool((b >> gx) & 1) for gx in range(8) for _ in range(2))
            g_rows.append(row_bits)
            g_rows.append(row_bits)
        glyphs[ch] = (16, 16, 16, 0, tuple(g_rows))
    return glyphs


def _fallback_soul():
    return tuple(tuple(c == "#" for c in row) for row in FALLBACK_SOUL_ROWS)


def _extract_runtime_assets(data_win_path: str):
    """Read only the player's FORM font and sprite for the theme, never for build verification.
    Returns (glyphs_dict, soul_mask_tuple) or (None, None) if unreadable."""
    if not data_win_path or not os.path.isfile(data_win_path):
        return None, None
    try:
        with open(data_win_path, "rb") as f:
            hdr = f.read(8)
            if len(hdr) < 8:
                return None, None
            magic, total_len = struct.unpack("<4sI", hdr)
            if magic != b"FORM" or total_len < 64:
                return None, None
            chunks = {}
            pos = 8
            while pos < total_len + 8:
                f.seek(pos)
                chdr = f.read(8)
                if len(chdr) < 8:
                    break
                tag, clen = struct.unpack("<4sI", chdr)
                chunks[tag.decode("latin1", errors="replace")] = (pos + 8, clen)
                pos += 8 + clen
            if not all(k in chunks for k in ("TXTR", "FONT", "SPRT")):
                return None, None

            def cstr(offset):
                if offset == 0:
                    return ""
                f.seek(offset)
                return f.read(64).split(b"\x00", 1)[0].decode("utf-8", errors="replace")

            txtr_pos, txtr_len = chunks["TXTR"]
            f.seek(txtr_pos)
            tcount = struct.unpack("<I", f.read(4))[0]
            if tcount == 0 or tcount > 4096:
                return None, None
            tptrs = struct.unpack("<%dI" % tcount, f.read(4 * tcount))
            esize = (tptrs[1] - tptrs[0]) if tcount >= 2 else 8
            blobs = []
            for p in tptrs:
                f.seek(p + esize - 4)
                blobs.append(struct.unpack("<I", f.read(4))[0])
            blobs.append(txtr_pos + txtr_len)

            # 1. Extract spr_heart (16x16 red SOUL)
            soul_mask = None
            sprt_pos, _ = chunks["SPRT"]
            f.seek(sprt_pos)
            scount = struct.unpack("<I", f.read(4))[0]
            if 0 < scount <= 16384:
                sptrs = struct.unpack("<%dI" % scount, f.read(4 * scount))
                for p in sptrs:
                    f.seek(p)
                    name_off, sw, sh = struct.unpack("<III", f.read(12))
                    if cstr(name_off) == "spr_heart":
                        f.seek(p + 56)
                        check = struct.unpack("<i", f.read(4))[0]
                        if check > 0:
                            tpag_off = struct.unpack("<I", f.read(4))[0]
                            f.seek(tpag_off)
                            sx, sy, tw, th, _, _, _, _, _, _, tex_id = struct.unpack("<10Hh", f.read(22))
                            if 0 <= tex_id < len(blobs) - 1 and tw > 0 and th > 0:
                                f.seek(blobs[tex_id])
                                png = f.read(blobs[tex_id + 1] - blobs[tex_id])
                                dec = _decode_png_rgba(png, crop_y0=sy, crop_h=th)
                                if dec:
                                    _, srows = dec
                                    raw_mask = [
                                        tuple((srows[gy][(sx + gx) * 4 + 3] > 80
                                               and (srows[gy][(sx + gx) * 4] > 80
                                                    or srows[gy][(sx + gx) * 4 + 1] > 80
                                                    or srows[gy][(sx + gx) * 4 + 2] > 80))
                                              for gx in range(tw))
                                        for gy in range(th)
                                    ]
                                    if tw == 16 and th == 16:
                                        soul_mask = tuple(raw_mask)
                                    else:
                                        # Fit inside a square without stretching a rectangular source sprite.
                                        fit_w = max(1, round(16 * tw / max(tw, th)))
                                        fit_h = max(1, round(16 * th / max(tw, th)))
                                        ox, oy = (16 - fit_w) // 2, (16 - fit_h) // 2
                                        soul_mask = tuple(tuple(
                                            (raw_mask[(gy - oy) * th // fit_h][(gx - ox) * tw // fit_w]
                                             if ox <= gx < ox + fit_w and oy <= gy < oy + fit_h else False)
                                            for gx in range(16)) for gy in range(16))
                        break

            # 2. Extract fnt_maintext (or fnt_main)
            glyphs = None
            font_pos, _ = chunks["FONT"]
            f.seek(font_pos)
            fcount = struct.unpack("<I", f.read(4))[0]
            if 0 < fcount <= 1024:
                fptrs = struct.unpack("<%dI" % fcount, f.read(4 * fcount))
                chosen_font = None
                for want_name in ("fnt_maintext", "fnt_main"):
                    for p in fptrs:
                        f.seek(p)
                        name_off, _ = struct.unpack("<II", f.read(8))
                        if cstr(name_off) == want_name:
                            chosen_font = p
                            break
                    if chosen_font is not None:
                        break
                if chosen_font is not None:
                    p = chosen_font
                    f.seek(p + 28)
                    tpag_off = struct.unpack("<I", f.read(4))[0]
                    list_start = p + 40
                    for trial in range(5):
                        f.seek(p + 40 + 4 * trial)
                        gc, fp = struct.unpack("<II", f.read(8))
                        if 0 < gc <= 0x10000 and fp == (p + 40 + 4 * trial + 4 + 4 * gc):
                            list_start = p + 40 + 4 * trial
                            break
                    f.seek(list_start)
                    gc = struct.unpack("<I", f.read(4))[0]
                    gptrs = struct.unpack("<%dI" % gc, f.read(4 * gc))
                    raw_glyphs = []
                    for gp in gptrs:
                        f.seek(gp)
                        raw_glyphs.append(struct.unpack("<5H2h", f.read(14)))
                    f.seek(tpag_off)
                    tp_x, tp_y, tp_w, tp_h, _, _, _, _, _, _, tex_id = struct.unpack("<10Hh", f.read(22))
                    if 0 <= tex_id < len(blobs) - 1 and tp_w > 0 and tp_h > 0:
                        f.seek(blobs[tex_id])
                        png = f.read(blobs[tex_id + 1] - blobs[tex_id])
                        dec = _decode_png_rgba(png, crop_y0=tp_y, crop_h=tp_h)
                        if dec:
                            _, frows = dec
                            gdict = {}
                            for ch, sx, sy, sw, sh, shift, ofs in raw_glyphs:
                                if ch > 127 or sy + sh > len(frows):
                                    continue
                                g_rows = []
                                for gy in range(sh):
                                    r = frows[sy + gy]
                                    x0 = tp_x + sx
                                    g_rows.append(tuple(r[(x0 + gx) * 4 + 3] > 127 for gx in range(sw)))
                                gdict[chr(ch)] = (sw, sh, max(1, shift), ofs, tuple(g_rows))
                            if "A" in gdict and "a" in gdict:
                                glyphs = gdict
            return glyphs, soul_mask
    except Exception:  # noqa: BLE001
        return None, None


class Theme:
    """Manages the bitmap pixel font, red SOUL sprite, and PhotoImage rendering for UNDERTALE-themed widgets."""

    def __init__(self, root, boring=False):
        self.root = root
        self.boring = boring
        self.glyphs = self._builtin_glyphs = _fallback_glyphs()
        self.soul_mask = _fallback_soul()
        self.loaded_folder = None
        self.runtime_loaded = False
        self._span_cache = {}
        self._text_cache = {}
        self._listeners = []
        self.blank = tk.PhotoImage(master=root, width=1, height=1)
        self.mono_font = self._pick_tk_font(10)
        self.mono_bold = self._pick_tk_font(10, bold=True)
        style = ttk.Style(root)
        preferred = "vista" if sys.platform == "win32" else "aqua" if sys.platform == "darwin" else "clam"
        self.native_theme = preferred if preferred in style.theme_names() else style.theme_use()
        probe = tk.Text(root)
        self.native_text = {key: probe.cget(key) for key in
                            ("background", "foreground", "insertbackground", "selectbackground", "selectforeground")}
        probe.destroy()
        # Keep the OS theme pristine, including clam on Linux. All game overrides belong to a child theme.
        if "builder-fun" not in style.theme_names():
            style.theme_create("builder-fun", parent="clam")
        self._configure_ttk()
        self.apply()

    @property
    def native(self):
        # The chosen style applies before discovery; player assets only replace the fallback font and heart.
        return self.boring

    def apply(self):
        style = ttk.Style(self.root)
        style.theme_use(self.native_theme if self.native else "builder-fun")
        bg = style.lookup("TFrame", "background") or "#d9d9d9"
        fg = style.lookup("TLabel", "foreground") or "#000000"
        field = style.lookup("TEntry", "fieldbackground") or (self.native_text["background"] if self.native else bg)
        selected = style.lookup("Treeview", "background", ("selected",)) or bg
        selected_fg = style.lookup("Treeview", "foreground", ("selected",)) or fg
        if self.native:
            # Treeview's default row height is fixed pixels even when the system font grows.
            style.configure("Treeview", rowheight=max(20, default_font(self.root).metrics("linespace") + 6))
        for key, value in (("background", field), ("foreground", fg),
                           ("selectBackground", selected), ("selectForeground", selected_fg),
                           ("font", "TkDefaultFont" if self.native else self.mono_font)):
            self.root.option_add("*TCombobox*Listbox." + key, value)
        self.root.configure(bg=bg)

        def visit(parent):
            for widget in parent.winfo_children():
                if isinstance(widget, tk.Frame):
                    widget.configure(bg=bg, highlightthickness=0 if self.native else
                                     (3 if getattr(widget, "_page_box", False) else 0))
                elif isinstance(widget, tk.Canvas):
                    widget.configure(bg=bg)
                elif isinstance(widget, tk.Text):
                    colors = self.native_text if self.native else dict(background=field, foreground=fg,
                                  insertbackground=fg, selectbackground=selected, selectforeground=selected_fg)
                    widget.configure(**colors,
                                     font="TkDefaultFont" if self.native else self.mono_font,
                                     highlightthickness=1 if self.native else 2,
                                     highlightbackground=fg, highlightcolor=fg)
                elif isinstance(widget, ttk.Combobox):
                    # An already opened dropdown is a Tcl-owned listbox; option_add only affects new ones.
                    popup = str(widget) + ".popdown.f.l"
                    if self.root.tk.call("winfo", "exists", popup):
                        self.root.tk.call(popup, "configure", "-background", field, "-foreground", fg,
                                          "-selectbackground", selected, "-selectforeground", selected_fg,
                                          "-font", "TkDefaultFont" if self.native else self.mono_font)
                visit(widget)
        visit(self.root)
        live = []
        for widget in self._listeners:
            if widget.winfo_exists():
                widget.refresh_theme()
                live.append(widget)
        self._listeners = live

    def _pick_tk_font(self, size, bold=False):
        try:
            fams = set(tkfont.families(self.root))
        except tk.TclError:
            fams = set()
        for cand in ("8bitoperator JVE", "Determination Mono", "Fixedsys", "Consolas", "Courier New", "Monaco",
                     "DejaVu Sans Mono"):
            if cand in fams:
                return (cand, size, "bold") if bold else (cand, size)
        return ("Courier", size, "bold") if bold else ("Courier", size)

    def _configure_ttk(self):
        style = ttk.Style(self.root)
        try:
            style.theme_use("builder-fun")
        except tk.TclError:
            pass
        style.configure(".", background="#000000", foreground="#ffffff", fieldbackground="#000000",
                        bordercolor="#ffffff", lightcolor="#ffffff", darkcolor="#ffffff",
                        troughcolor="#000000", focuscolor="#ffff00", font=self.mono_font)
        style.configure("TFrame", background="#000000", borderwidth=0)
        style.configure("Box.TFrame", background="#000000", bordercolor="#ffffff", lightcolor="#ffffff",
                        darkcolor="#ffffff", relief="solid", borderwidth=2)
        style.configure("TLabel", background="#000000", foreground="#ffffff", borderwidth=0, padding=0)
        style.configure("TSeparator", background="#ffffff")
        style.configure("TLabelframe", background="#000000", foreground="#ffff00", bordercolor="#ffffff",
                        lightcolor="#ffffff", darkcolor="#ffffff", borderwidth=2, relief="solid")
        style.configure("TLabelframe.Label", background="#000000", foreground="#ffff00", font=self.mono_bold)

        # Strip native radio/check/button indicators so our PhotoImage bitmap + red SOUL render cleanly.
        style.layout("Ut.TButton", [
            ("Button.padding", {"sticky": "nswe", "children": [("Button.label", {"sticky": "nswe"})]})
        ])
        style.configure("Ut.TButton", background="#000000", foreground="#ff7f27", borderwidth=0, padding=0,
                        relief="flat", focuscolor="#000000")
        style.map("Ut.TButton", background=[("active", "#000000"), ("disabled", "#000000")])

        style.layout("Ut.TRadiobutton", [
            ("Radiobutton.padding", {"sticky": "nswe", "children": [("Radiobutton.label", {"sticky": "nswe"})]})
        ])
        style.configure("Ut.TRadiobutton", background="#000000", foreground="#ffffff", borderwidth=0, padding=(0, 2),
                        relief="flat", focuscolor="#000000")
        style.map("Ut.TRadiobutton", background=[("active", "#000000"), ("selected", "#000000"),
                                                 ("disabled", "#000000")])

        style.layout("Ut.TCheckbutton", [
            ("Checkbutton.padding", {"sticky": "nswe", "children": [("Checkbutton.label", {"sticky": "nswe"})]})
        ])
        style.configure("Ut.TCheckbutton", background="#000000", foreground="#ffffff", borderwidth=0, padding=(0, 2),
                        relief="flat", focuscolor="#000000")
        style.map("Ut.TCheckbutton", background=[("active", "#000000"), ("selected", "#000000"),
                                                 ("disabled", "#000000")])

        row_height = max(24, tkfont.Font(root=self.root, font=self.mono_font).metrics("linespace") + 8)
        style.configure("Treeview", background="#000000", foreground="#ffffff", fieldbackground="#000000",
                        bordercolor="#ffffff", lightcolor="#ffffff", darkcolor="#ffffff", borderwidth=2,
                        rowheight=row_height, font=self.mono_font)
        style.configure("Treeview.Heading", background="#000000", foreground="#ffff00", bordercolor="#ffffff",
                        lightcolor="#ffffff", darkcolor="#ffffff", relief="solid", font=self.mono_bold)
        style.map("Treeview",
                  background=[("selected", "#220000")],
                  foreground=[("selected", "#ffff00")])
        style.map("Treeview.Heading", background=[("active", "#111111")])

        style.configure("TCombobox", fieldbackground="#000000", background="#000000", foreground="#ffffff",
                        arrowcolor="#ff7f27", bordercolor="#ffffff", lightcolor="#ffffff", darkcolor="#ffffff",
                        selectbackground="#000000", selectforeground="#ffff00")
        style.map("TCombobox",
                  fieldbackground=[("readonly", "#000000"), ("disabled", "#000000")],
                  foreground=[("readonly", "#ffffff"), ("disabled", "#555555")],
                  selectbackground=[("readonly", "#000000")],
                  selectforeground=[("readonly", "#ffff00")])
        self.root.option_add("*TCombobox*Listbox.background", "#000000")
        self.root.option_add("*TCombobox*Listbox.foreground", "#ffffff")
        self.root.option_add("*TCombobox*Listbox.selectBackground", "#220000")
        self.root.option_add("*TCombobox*Listbox.selectForeground", "#ffff00")

        style.configure("Horizontal.TProgressbar", troughcolor="#000000", background="#ffff00",
                        bordercolor="#ffffff", lightcolor="#ffffff", darkcolor="#ffffff", thickness=20)
        style.configure("Vertical.TScrollbar", troughcolor="#000000", background="#000000", bordercolor="#ffffff",
                        lightcolor="#000000", darkcolor="#000000", arrowcolor="#ff7f27", gripcount=0)
        style.map("Vertical.TScrollbar", background=[("active", "#000000")], arrowcolor=[("active", "#ffff00")])

    def register(self, widget):
        self._listeners.append(widget)

    def load_from_game_dir(self, folder: str | None):
        if not folder or folder == self.loaded_folder:
            return
        self.loaded_folder = folder
        # Native depots may supply the theme; the build's Windows-only allow-list is unchanged.
        candidates = [os.path.join(folder, "data.win"), *discover.native_data_files(folder)]
        glyphs, soul = None, None
        for cand in candidates:
            glyphs, soul = _extract_runtime_assets(cand)
            if glyphs or soul:
                break
        if glyphs and soul:
            self.glyphs = glyphs
            self.soul_mask = soul
            self.runtime_loaded = True
            self._span_cache.clear()
            self._text_cache.clear()
            self.apply()
        else:
            self.runtime_loaded = False
            self.apply()

    def can_render_bitmap(self, text: str) -> bool:
        return all((ch in ("\n", "\r", "\t") or ch in self.glyphs) for ch in text)

    def _glyph_spans(self, ch: str, color: tuple, scale: int):
        key = (ch, color, scale)
        cached = self._span_cache.get(key)
        if cached is not None:
            return cached
        g = self.glyphs.get(ch) or self.glyphs.get("?") or self.glyphs.get(" ")
        if not g:
            res = (8 * scale, 16 * scale, 8 * scale, 0, ())
            self._span_cache[key] = res
            return res
        sw, sh, shift, ofs, g_rows = g
        cbytes = bytes(color)
        spans = []
        for gy, row_bits in enumerate(g_rows):
            x = 0
            n = len(row_bits)
            while x < n:
                if not row_bits[x]:
                    x += 1
                    continue
                x_end = x + 1
                while x_end < n and row_bits[x_end]:
                    x_end += 1
                run_px = (x_end - x) * scale
                sbytes = cbytes * run_px
                x0_s = (ofs + x) * scale
                for sy in range(scale):
                    spans.append((gy * scale + sy, x0_s, sbytes))
                x = x_end
        res = (sw * scale, sh * scale, shift * scale, ofs * scale, tuple(spans))
        self._span_cache[key] = res
        return res

    def line_height(self, scale: int = 1) -> int:
        return 18 * scale

    def measure_line(self, text: str, scale: int = 1) -> int:
        w = 0
        for ch in text:
            _, _, shift_s, _, _ = self._glyph_spans(ch, WHITE, scale)
            w += shift_s
        return max(1, w)

    def _split_long_word(self, word: str, wrap: int, scale: int) -> list[str]:
        """A word wider than the line (a long path) breaks after a path separator, else between characters."""
        if self.measure_line(word, scale) <= wrap:
            return [word]
        pieces, cur, cur_w, cut = [], "", 0, -1
        for ch in word:
            cw = self._glyph_spans(ch, WHITE, scale)[2]
            if cur and cur_w + cw > wrap:
                if cut > 0:
                    pieces.append(cur[:cut])
                    cur = cur[cut:]
                else:
                    pieces.append(cur)
                    cur = ""
                cur_w = self.measure_line(cur, scale) if cur else 0
                cut = -1
            cur += ch
            cur_w += cw
            if ch in "\\/":
                cut = len(cur)
        if cur:
            pieces.append(cur)
        return pieces

    def wrap_lines(self, text: str, wrap: int, scale: int = 1) -> list[str]:
        if not text:
            return []
        wrap = max(80, wrap)
        space_w = self._glyph_spans(" ", WHITE, scale)[2]
        out = []
        for para in text.split("\n"):
            if not para:
                out.append("")
                continue
            words = []
            for word in para.split(" "):
                words.extend(self._split_long_word(word, wrap, scale))
            cur = ""
            cur_w = 0
            for word in words:
                ww = self.measure_line(word, scale)
                if not cur:
                    cur = word
                    cur_w = ww
                elif cur_w + space_w + ww <= wrap:
                    cur += " " + word
                    cur_w += space_w + ww
                else:
                    out.append(cur)
                    cur = word
                    cur_w = ww
            if cur or not words:
                out.append(cur)
        return out

    def draw_text_buf(self, buf: bytearray, bw: int, bh: int, x: int, y: int, text: str, color=WHITE, scale: int = 1):
        cx = x
        for ch in text:
            _, _, shift_s, _, spans = self._glyph_spans(ch, color, scale)
            for gy_s, x0_s, sbytes in spans:
                py = y + gy_s
                if 0 <= py < bh:
                    px = cx + x0_s
                    if px >= 0 and px * 3 + len(sbytes) <= bw * 3:
                        idx = (py * bw + px) * 3
                        buf[idx:idx + len(sbytes)] = sbytes
            cx += shift_s

    def draw_soul_buf(self, buf: bytearray, bw: int, bh: int, x: int, y: int, color=RED):
        cbytes = bytes(color)
        for sy, row in enumerate(self.soul_mask):
            py = y + sy
            if not (0 <= py < bh):
                continue
            for sx, on in enumerate(row):
                if on:
                    px = x + sx
                    if 0 <= px < bw:
                        idx = (py * bw + px) * 3
                        buf[idx:idx + 3] = cbytes

    def draw_box_buf(self, buf: bytearray, bw: int, bh: int, x0: int, y0: int, x1: int, y1: int, color=WHITE,
                     thick: int = 2):
        cbytes = bytes(color)
        hline = cbytes * max(0, x1 - x0)
        for t in range(thick):
            for py in (y0 + t, y1 - 1 - t):
                if 0 <= py < bh and x0 < x1:
                    idx = (py * bw + x0) * 3
                    buf[idx:idx + len(hline)] = hline
        vbytes = cbytes * thick
        for py in range(y0, y1):
            if 0 <= py < bh:
                if 0 <= x0 and x0 + thick <= bw:
                    idx = (py * bw + x0) * 3
                    buf[idx:idx + len(vbytes)] = vbytes
                if 0 <= x1 - thick and x1 <= bw:
                    idx = (py * bw + (x1 - thick)) * 3
                    buf[idx:idx + len(vbytes)] = vbytes

    def render_text_image(self, text: str, wrap: int = WRAP, scale: int = 1, color=WHITE) -> tk.PhotoImage | str:
        if not text:
            return ""
        ckey = (text, wrap, scale, color)
        cached = self._text_cache.get(ckey)
        if cached is not None:
            return cached
        lines = self.wrap_lines(text, wrap, scale)
        if not lines:
            return ""
        lh = self.line_height(scale)
        # The built-in font (before the game is found) is 16 px tall and wraps sooner than the game's, so its blank
        # lines between paragraphs are half a line: the About notice then fits the smallest window (680x560).
        # The game's font keeps whole blank lines.
        gap = lh // 2 if self.glyphs is self._builtin_glyphs else lh
        tops, y = [], 0
        for line in lines:
            tops.append(y)
            y += lh if line else gap
        w = max(4, max(self.measure_line(line, scale) for line in lines) + 2)
        h = max(4, y)
        buf = bytearray(w * h * 3)
        for top, line in zip(tops, lines):
            self.draw_text_buf(buf, w, h, 0, top + 1, line, color=color, scale=scale)
        ppm = ("P6\n%d %d\n255\n" % (w, h)).encode("ascii") + bytes(buf)
        img = tk.PhotoImage(master=self.root, data=ppm)
        if len(self._text_cache) > 512:
            self._text_cache.clear()
        self._text_cache[ckey] = img
        return img

    def render_button_image(self, text: str, active: bool = False, disabled: bool = False,
                            show_soul: bool = False, scale: int = 1) -> tk.PhotoImage:
        color = DARK_GRAY if disabled else (YELLOW if active else ORANGE)
        tw = self.measure_line(text, scale)
        lh = self.line_height(scale)
        # The SOUL's place is always kept, so hovering or focusing a button never changes its size (a size change
        # moved its neighbours in the nav bar, and the old width was left drawn under the next button).
        soul_space = 22
        pad_x = 10
        pad_y = 6
        w = max(48, tw + soul_space + pad_x * 2)
        h = max(28, lh + pad_y * 2)
        buf = bytearray(w * h * 3)
        self.draw_box_buf(buf, w, h, 0, 0, w, h, color=color, thick=2)
        # Idle text sits in the middle; the SOUL takes the left side when the button is selected.
        tx = pad_x + soul_space if show_soul else (w - tw) // 2
        ty = (h - lh) // 2 + 1
        if show_soul:
            sy = (h - 16) // 2
            self.draw_soul_buf(buf, w, h, pad_x, sy, color=DARK_GRAY if disabled else RED)
        self.draw_text_buf(buf, w, h, tx, ty, text, color=color, scale=scale)
        ppm = ("P6\n%d %d\n255\n" % (w, h)).encode("ascii") + bytes(buf)
        return tk.PhotoImage(master=self.root, data=ppm)

    def render_choice_image(self, text: str, selected: bool, is_check: bool, disabled: bool = False,
                            hovered: bool = False, scale: int = 1, wrap: int = WRAP) -> tk.PhotoImage:
        color = DARK_GRAY if disabled else (YELLOW if (selected or hovered) else WHITE)
        lines = self.wrap_lines(text, wrap - 36, scale) or [""]
        lh = self.line_height(scale)
        tw = max(self.measure_line(line, scale) for line in lines)
        icon_w = 24
        w = max(32, icon_w + tw + 4)
        h = max(20, len(lines) * lh + 2)
        buf = bytearray(w * h * 3)
        iy = max(0, (lh - 16) // 2)
        if selected:
            self.draw_soul_buf(buf, w, h, 2, iy, color=DARK_GRAY if disabled else RED)
        elif is_check:
            self.draw_box_buf(buf, w, h, 3, iy + 2, 15, iy + 14, color=DARK_GRAY if disabled else WHITE, thick=2)
        else:
            # Small pixel diamond for unselected radio option
            self.draw_box_buf(buf, w, h, 7, iy + 5, 13, iy + 11, color=DARK_GRAY if disabled else GRAY, thick=1)
        for i, line in enumerate(lines):
            self.draw_text_buf(buf, w, h, icon_w, i * lh + 1, line, color=color, scale=scale)
        ppm = ("P6\n%d %d\n255\n" % (w, h)).encode("ascii") + bytes(buf)
        return tk.PhotoImage(master=self.root, data=ppm)

    def render_progressbar_image(self, width: int, height: int, value: float, maximum: float = 100.0,
                                 indeterminate: bool = False, step: int = 0) -> tk.PhotoImage:
        w = max(60, int(width))
        h = max(18, int(height))
        buf = bytearray(w * h * 3)
        self.draw_box_buf(buf, w, h, 0, 0, w, h, color=WHITE, thick=2)
        inner_w = w - 6
        frac = max(0.0, min(1.0, float(value) / max(1.0, float(maximum))))
        fill_w = int(round(inner_w * frac))
        if not indeterminate and fill_w > 0:
            row_fill = bytes(YELLOW) * fill_w
            for py in range(3, h - 3):
                idx = (py * w + 3) * 3
                buf[idx:idx + len(row_fill)] = row_fill
        # Draw the red SOUL marker at the current progress position
        if indeterminate:
            pos = (step * 12) % max(1, inner_w - 16)
            sx = 3 + pos
        else:
            sx = 3 + max(0, min(inner_w - 16, fill_w - 8))
        sy = max(1, (h - 16) // 2)
        self.draw_soul_buf(buf, w, h, sx, sy, color=RED)
        ppm = ("P6\n%d %d\n255\n" % (w, h)).encode("ascii") + bytes(buf)
        return tk.PhotoImage(master=self.root, data=ppm)


def _find_theme(widget) -> Theme | None:
    w = widget
    while w is not None:
        t = getattr(w, "_ut_theme", None)
        if t is not None:
            return t
        w = getattr(w, "master", None)
    return None


def _find_page(widget):
    w = widget
    while w is not None:
        p = getattr(w, "_page", None)
        if p is not None:
            return p
        w = getattr(w, "master", None)
    return None


class Wrapping:
    """Text that wraps to the width of its page: the page sets the width whenever the window is resized."""

    def _join_page(self, parent):
        self._wrap_page = _find_page(parent)
        if self._wrap_page is not None:
            self._wrap_page.wrappers.append(self)
            width = self._wrap_page.wrap_width(self)
            if width:
                self._wrap = width

    def set_wrap(self, width):
        width = max(120, int(width))
        if width != self._wrap:
            self._wrap = width
            self.refresh_theme()


class PixelLabel(Wrapping, ttk.Label):
    """A ttk.Label subclass that renders its text using the bitmap pixel font while keeping cget('text') intact.

    With url, it is a link: orange in the fun look, underlined in the system look, opened with a click or Enter."""

    def __init__(self, parent, text="", bold=False, wrap=WRAP, scale=1, color=None, url=None, **kw):
        self._theme = _find_theme(parent)
        self._raw_text = str(text or "")
        self._bold = bold
        self._wrap = wrap
        self._scale = scale
        self._url = url
        self._color = color or (ORANGE if url else (YELLOW if bold else WHITE))
        self._img = None
        if url:
            kw.setdefault("cursor", "hand2")
            kw.setdefault("takefocus", 1)
        super().__init__(parent, text=self._raw_text, **kw)
        if url:
            for seq in ("<Button-1>", "<Return>", "<space>"):
                self.bind(seq, self._open_link)
            self.bind("<FocusIn>", lambda _e: self._focus(True))
            self.bind("<FocusOut>", lambda _e: self._focus(False))
        self._join_page(parent)
        if self._theme is not None:
            self._theme.register(self)
            self._render()

    def _open_link(self, event):
        if KeyWatch.note(event) == "break":     # a held key's repeat never opens the page again
            return "break"
        webbrowser.open(self._url)
        return None

    def _focus(self, on):
        if self._url and not self._bold:
            self._color = YELLOW if on else ORANGE
            self._render()

    def _link_font(self):
        font = getattr(self._theme, "_link_font", None)
        if font is None:
            font = default_font(self._theme.root).copy()
            font.configure(underline=True)
            self._theme._link_font = font
        return font

    def _collapse(self, empty):
        """An empty label takes no room (no text line and no padding) but keeps its place in the page."""
        if self.winfo_manager() != "pack":
            return
        if empty and getattr(self, "_saved_pady", None) is None:
            self._saved_pady = self.pack_info().get("pady", 0)
            self.pack_configure(pady=0)
        elif not empty and getattr(self, "_saved_pady", None) is not None:
            self.pack_configure(pady=self._saved_pady)
            self._saved_pady = None

    def _render(self):
        if self._theme is None:
            return
        self._collapse(not self._raw_text)
        if not self._raw_text:
            self._img = None
            super().configure(text="", image=self._theme.blank, compound="image", wraplength=0)
            return
        if self._theme.native:
            self._img = None
            super().configure(text=self._raw_text, image="", compound="none",
                              foreground="#0645ad" if self._url else "",
                              font=self._link_font() if self._url else "TkDefaultFont", wraplength=self._wrap)
            return
        if not self._theme.can_render_bitmap(self._raw_text):
            self._img = None
            fg = "#%02x%02x%02x" % self._color
            font = "TkDefaultFont"
            super().configure(text=self._raw_text, image="", compound="none", foreground=fg, font=font,
                              wraplength=self._wrap)
            return
        super().configure(foreground="", font="", wraplength=0)
        self._img = self._theme.render_text_image(self._raw_text, wrap=self._wrap, scale=self._scale,
                                                  color=self._color)
        super().configure(text=self._raw_text, image=self._img, compound="image")

    def refresh_theme(self):
        self._render()

    def configure(self, cnf=None, **kw):
        if isinstance(cnf, dict):
            kw = {**cnf, **kw}
            cnf = None
        if not cnf and not kw:
            return super().configure()
        if "text" in kw:
            self._raw_text = str(kw.pop("text") or "")
            if kw:
                super().configure(cnf, **kw)
            self._render()
            return None
        return super().configure(cnf, **kw)

    config = configure


class UtButton(ttk.Button):
    """An UNDERTALE-styled action button (orange idle border, yellow active border + red SOUL marker)."""

    def __init__(self, parent, text="", command=None, primary=False, scale=1, **kw):
        self._theme = _find_theme(parent)
        self._raw_text = str(text or "")
        self._primary = primary
        self._scale = scale
        self._hovered = False
        self._img = None
        super().__init__(parent, text=self._raw_text, command=command, style="Ut.TButton", **kw)
        self.bind("<KeyPress>", KeyWatch.note, add="+")
        self.bind("<Enter>", self._on_enter)
        self.bind("<Leave>", self._on_leave)
        self.bind("<FocusIn>", self._on_enter)
        self.bind("<FocusOut>", self._on_leave)
        if self._theme is not None:
            self._theme.register(self)
            self._render()

    def _on_enter(self, _e=None):
        self._hovered = True
        self._render()

    def _on_leave(self, _e=None):
        self._hovered = False
        self._render()

    def _render(self):
        if self._theme is None:
            return
        if self._theme.native:
            self._img = None
            super().configure(style="TButton", text=self._raw_text, image="", compound="none")
            return
        super().configure(style="Ut.TButton")
        disabled = self.instate(["disabled"])
        active = (self._hovered or self._primary) and not disabled
        show_soul = (self._hovered or self._primary) and not disabled
        self._img = self._theme.render_button_image(self._raw_text, active=active, disabled=disabled,
                                                    show_soul=show_soul, scale=self._scale)
        super().configure(text=self._raw_text, image=self._img, compound="image")

    def refresh_theme(self):
        self._render()

    def state(self, statespec=None):
        res = super().state(statespec)
        if statespec is not None:
            self._render()
        return res

    def configure(self, cnf=None, **kw):
        if isinstance(cnf, dict):
            kw = {**cnf, **kw}
            cnf = None
        if not cnf and not kw:
            return super().configure()
        if "text" in kw:
            self._raw_text = str(kw.pop("text") or "")
            if kw:
                super().configure(cnf, **kw)
            self._render()
            return None
        res = super().configure(cnf, **kw)
        self._render()
        return res

    config = configure


class UtRadio(Wrapping, ttk.Radiobutton):
    """A ttk.Radiobutton subclass that uses the red SOUL sprite as its selection marker."""

    def __init__(self, parent, text="", value="", variable=None, command=None, scale=1, **kw):
        self._theme = _find_theme(parent)
        self._raw_text = str(text or "")
        self._value = value
        self._var = variable
        self._scale = scale
        self._hovered = False
        self._img = None
        self._wrap = WRAP
        super().__init__(parent, text=self._raw_text, value=value, variable=variable, command=command,
                         style="Ut.TRadiobutton", **kw)
        self.bind("<KeyPress>", KeyWatch.note, add="+")  # a held key's repeat never acts
        for seq in ("<Enter>", "<FocusIn>"):
            self.bind(seq, self._on_enter, add="+")
        for seq in ("<Leave>", "<FocusOut>"):
            self.bind(seq, self._on_leave, add="+")
        self._join_page(parent)
        if variable is not None:
            variable.trace_add("write", self._on_var)
        if self._theme is not None:
            self._theme.register(self)
            self._sync_and_render()

    def _on_enter(self, _e=None):
        self._hovered = True
        self._sync_and_render()

    def _on_leave(self, _e=None):
        self._hovered = False
        self._sync_and_render()

    def _on_var(self, *_a):
        try:
            if self.winfo_exists():
                self._sync_and_render()
        except tk.TclError:
            pass

    def _is_selected(self) -> bool:
        if self._var is not None:
            try:
                return str(self._var.get()) == str(self._value)
            except tk.TclError:
                pass
        return self.instate(["selected"])

    def _sync_and_render(self):
        sel = self._is_selected()
        try:
            super().state(["selected"] if sel else ["!selected"])
        except tk.TclError:
            pass
        if self._theme is None:
            return
        if self._theme.native:
            self._img = None
            super().configure(style="TRadiobutton", text=self._raw_text, image="", compound="none")
            return
        super().configure(style="Ut.TRadiobutton")
        disabled = self.instate(["disabled"])
        self._img = self._theme.render_choice_image(self._raw_text, selected=sel, is_check=False,
                                                    disabled=disabled, hovered=self._hovered, scale=self._scale,
                                                    wrap=self._wrap)
        super().configure(text=self._raw_text, image=self._img, compound="image")

    def refresh_theme(self):
        self._sync_and_render()

    def state(self, statespec=None):
        res = super().state(statespec)
        if statespec is not None:
            self._sync_and_render()
        return res

    def configure(self, cnf=None, **kw):
        if isinstance(cnf, dict):
            kw = {**cnf, **kw}
            cnf = None
        if not cnf and not kw:
            return super().configure()
        if 'text' in kw:
            self._raw_text = str(kw.pop('text') or '')
        result = super().configure(cnf, **kw) if cnf or kw else None
        self._sync_and_render()
        return result

    config = configure


class UtCheck(Wrapping, ttk.Checkbutton):
    """A ttk.Checkbutton subclass that uses the red SOUL sprite as its selection marker."""

    def __init__(self, parent, text="", variable=None, command=None, scale=1, **kw):
        self._theme = _find_theme(parent)
        self._raw_text = str(text or "")
        self._var = variable
        self._scale = scale
        self._hovered = False
        self._img = None
        self._wrap = WRAP
        super().__init__(parent, text=self._raw_text, variable=variable, command=command,
                         style="Ut.TCheckbutton", **kw)
        self.bind("<KeyPress>", KeyWatch.note, add="+")  # a held key's repeat never acts
        for seq in ("<Enter>", "<FocusIn>"):
            self.bind(seq, self._on_enter, add="+")
        for seq in ("<Leave>", "<FocusOut>"):
            self.bind(seq, self._on_leave, add="+")
        self._join_page(parent)
        if variable is not None:
            variable.trace_add("write", self._on_var)
        if self._theme is not None:
            self._theme.register(self)
            self._sync_and_render()

    def _on_enter(self, _e=None):
        self._hovered = True
        self._sync_and_render()

    def _on_leave(self, _e=None):
        self._hovered = False
        self._sync_and_render()

    def _on_var(self, *_a):
        try:
            if self.winfo_exists():
                self._sync_and_render()
        except tk.TclError:
            pass

    def _is_selected(self) -> bool:
        if self._var is not None:
            try:
                return bool(self._var.get())
            except tk.TclError:
                pass
        return self.instate(["selected"])

    def _sync_and_render(self):
        sel = self._is_selected()
        try:
            super().state(["selected"] if sel else ["!selected"])
        except tk.TclError:
            pass
        if self._theme is None:
            return
        if self._theme.native:
            self._img = None
            super().configure(style="TCheckbutton", text=self._raw_text, image="", compound="none")
            return
        super().configure(style="Ut.TCheckbutton")
        disabled = self.instate(["disabled"])
        self._img = self._theme.render_choice_image(self._raw_text, selected=sel, is_check=True,
                                                    disabled=disabled, hovered=self._hovered, scale=self._scale,
                                                    wrap=self._wrap)
        super().configure(text=self._raw_text, image=self._img, compound="image")

    def refresh_theme(self):
        self._sync_and_render()

    def state(self, statespec=None):
        res = super().state(statespec)
        if statespec is not None:
            self._sync_and_render()
        return res


class UtProgressbar(ttk.Progressbar):
    """Native bar or persistent canvas items, with one animation timer."""

    def __init__(self, parent, mode="determinate", length=500, maximum=100, **kw):
        self._theme = _find_theme(parent)
        self._length, self._height = length, 20
        self._max, self._mode = maximum, mode
        self._val = float(kw.get("value", 0.0))
        self._step = 0
        self._animating = self._native_running = False
        self._interval = None
        self._tick_id = self._render_id = None
        self._img = self._mask = self._geometry = None
        self._overlay_visible = False
        super().__init__(parent, mode=mode, length=length, maximum=maximum, **kw)
        # Window-level cancellation must unregister from the same command owner.
        self._clock = self.winfo_toplevel()
        self._overlay = tk.Canvas(self, bd=0, highlightthickness=0, bg="#000000")
        self._overlay._lint_skip = True
        self._fill = self._overlay.create_rectangle(0, 0, 0, 0, fill="#ffff00", width=0)
        self._border = self._overlay.create_rectangle(0, 0, 0, 0, outline="#ffffff", width=2)
        self._heart = self._overlay.create_image(0, 0, anchor="nw")
        self.bind("<Configure>", self._resized, add="+")
        self.bind("<Destroy>", self._destroyed, add="+")
        if self._theme is not None:
            self._theme.register(self)
        self._render()

    def _resized(self, event):
        if (event.width, event.height) != (self._length, self._height):
            self._length, self._height = event.width, event.height
            self._request_render()

    def _request_render(self):
        # Coalesce worker bursts and repeated polling; never enter a nested Tk update.
        if self._render_id is None:
            self._render_id = self._clock.after(50, self._render)

    def _render(self):
        if self._render_id is not None:
            self._clock.after_cancel(self._render_id)
            self._render_id = None
        if self._theme is None or self._theme.native:
            if self._overlay_visible:
                self._overlay.place_forget()
                self._overlay_visible = False
            if not self._native_running:
                super().configure(value=self._val)
            return
        if not self._overlay_visible:
            self._overlay.configure(bg="#000000")
            self._overlay.place(x=0, y=0, relwidth=1, relheight=1)
            self._overlay_visible = True
        mask = tuple(tuple(row) for row in self._theme.soul_mask)
        if mask != self._mask:
            # Transparent pixels let the existing fill show through the sprite.
            image = tk.PhotoImage(master=self, width=16, height=16)
            for y, row in enumerate(mask[:16]):
                for x, on in enumerate(row[:16]):
                    if on:
                        image.put("#ff0000", (x, y))
            self._overlay.itemconfigure(self._heart, image=image)
            self._img, self._mask = image, mask
        w, h = max(60, int(self._length)), max(18, int(self._height))
        inner = w - 6
        fraction = max(0.0, min(1.0, self._val / max(1.0, float(self._max))))
        fill = int(round(inner * fraction))
        moving = self._mode == "indeterminate" and self._animating
        x = 3 + ((self._step * 12) % max(1, inner - 16) if moving else
                 max(0, min(inner - 16, fill - 8)))
        geometry = (w, h, fill, x, max(1, (h - 16) // 2), moving)
        if geometry == self._geometry:
            return
        previous = self._geometry
        if previous is None or previous[:2] != geometry[:2]:
            self._overlay.coords(self._border, 1, 1, w-1, h-1)
        if previous is None or previous[:3] != geometry[:3]:
            self._overlay.coords(self._fill, 3, 3, 3+fill, h-3)
        if previous is None or previous[2] != fill or previous[5] != moving:
            self._overlay.itemconfigure(self._fill, state="hidden" if moving or not fill else "normal")
        if previous is None or previous[3:5] != geometry[3:5]:
            self._overlay.coords(self._heart, x, geometry[4])
        self._geometry = geometry

    def refresh_theme(self):
        self._render()
        self._sync_timer()

    def _cancel_tick(self):
        if self._tick_id is not None:
            self._clock.after_cancel(self._tick_id)
            self._tick_id = None

    def _sync_timer(self):
        native = self._theme is None or self._theme.native
        if native and self._animating:
            self._cancel_tick()
            if not self._native_running:
                super().start(self._interval)
                self._native_running = True
        else:
            if self._native_running:
                super().stop()
                self._native_running = False
            if self._animating and self._mode == "indeterminate":
                if self._tick_id is None:
                    self._tick_id = self._clock.after(120, self._tick)
            else:
                self._cancel_tick()

    def start(self, interval=None):
        self._interval = interval
        self._animating = True
        self._sync_timer()
        self._request_render()

    def _tick(self):
        self._tick_id = None
        if not self._animating or not self.winfo_exists():
            return
        self._step += 1
        self._render()
        self._sync_timer()

    def stop(self):
        self._animating = False
        self._sync_timer()
        self._request_render()

    def _destroyed(self, event):
        if event.widget is self:
            self._cancel_tick()
            if self._render_id is not None:
                self._clock.after_cancel(self._render_id)
                self._render_id = None

    def configure(self, cnf=None, **kw):
        if isinstance(cnf, dict):
            kw = {**cnf, **kw}
            cnf = None
        if not cnf and not kw:
            return super().configure()
        before = (self._mode, self._val, self._max)
        if "mode" in kw:
            self._mode = kw["mode"]
            if self._mode != "indeterminate":
                self._animating = False
        if "value" in kw:
            self._val = float(kw["value"])
        if "maximum" in kw:
            self._max = float(kw["maximum"])
        result = super().configure(cnf, **kw)
        if before != (self._mode, self._val, self._max):
            self._sync_timer()
            self._request_render()
        return result

    config = configure


# ---------------------------------------------------------------------------------------------------------------------

class Options:
    def __init__(self, game=None, work=None, wifi=False, toolchain_dry_run=False, autopilot=None, devkitpro=None,
                 probes=None, confirm_overwrite=None, boring=None, folder=None, confirm_install=None):
        self.boring = boring
        self.confirm_install = confirm_install
        self.game = game
        self.work = work or pipeline.default_work_dir()
        self.wifi = wifi
        self.toolchain_dry_run = toolchain_dry_run
        self.autopilot = autopilot
        self.devkitpro = devkitpro
        self.probes = probes or discover.Probes()
        self.confirm_overwrite = confirm_overwrite
        self.folder = folder

    @property
    def tools_dir(self):
        return os.path.join(self.work, "tools", "bin")

    @property
    def downloads_dir(self):
        return os.path.join(self.work, "downloads")


class State:
    def __init__(self, game):
        self.game = game
        self.folder = None
        self.verdict = None
        self.tc = None
        self.targets = ["n3ds", "o3ds"]
        self.outputs = ["3dsx"]
        self.parts = None
        self.openh264 = False
        self.artifacts = []
        self.records = []
        self.build_ok = False
        self.build_key = None
        self.delivered = False
        self.delivered_key = None  # DeliverPage.delivery_key() of the copy that succeeded (AUD-05).
        self.delivery_visit = 0  # A replacement card can reuse the same path and delivery key.
        self.inputs = 0  # Counts builds and game folder choices: each one makes earlier copies stale (R175 F4).
        self.deliver_how = "sd"
        self.sd_root = None
        self.files_root = None
        self.ftp_console = None
        self.save_scan = None
        self.save_result = None
        self.save_how = None
        self.save_destination = None
        self.save_receipt = None
        self.saves_done = False
        self.texture_result = None

    def build_input_key(self):
        fields = ('root', 'bash', 'platform', 'makerom', 'bannertool', 'package_versions', 'compiler_info')
        tools = tuple(json.dumps(getattr(self.tc, field, None), sort_keys=True, default=str) for field in fields)
        return self.game, self.folder, self.inputs, tools

    def invalidate_build(self):
        self.build_ok = False
        self.build_key = None
        self.artifacts, self.records = [], []
        self.delivered = False

    def new_delivery(self):
        self.delivery_visit += 1
        self.texture_result = None
        self.save_result = self.save_how = self.save_destination = self.save_receipt = None
        self.saves_done = False

    def delivery_context(self):
        destination = (self.sd_root if self.deliver_how == 'sd' else
                       self.ftp_console if self.deliver_how == 'wifi' else self.files_root)
        if destination and self.deliver_how != 'wifi':
            destination = os.path.normcase(os.path.normpath(destination))
        return self.delivery_visit, self.deliver_how, destination, self.delivered_key

    def current_save_result(self):
        receipt = self.delivery_context(), self.save_how, self.save_destination
        return self.save_result if self.save_receipt == receipt else None

    @property
    def product(self):
        return products.PRODUCTS[self.game]


def label(parent, text="", bold=False, wrap=WRAP, scale=1, color=None, **kw):
    w = PixelLabel(parent, text=text, bold=bold, wrap=wrap, scale=scale, color=color, **kw)
    w.pack(anchor="w", pady=(0, 6))
    w._collapse(not w._raw_text)
    return w


def button(parent, text="", command=None, primary=False, scale=1, **kw):
    return UtButton(parent, text=text, command=command, primary=primary, scale=scale, **kw)


def radiobutton(parent, text="", value="", variable=None, command=None, scale=1, **kw):
    return UtRadio(parent, text=text, value=value, variable=variable, command=command, scale=scale, **kw)


def checkbutton(parent, text="", variable=None, command=None, scale=1, **kw):
    return UtCheck(parent, text=text, variable=variable, command=command, scale=scale, **kw)


def progressbar(parent, mode="determinate", length=500, maximum=100, **kw):
    return UtProgressbar(parent, mode=mode, length=length, maximum=maximum, **kw)


def text_of(widget):
    """Every label's text under widget, for the autopilot's page summary."""
    out = []
    for w in widget.winfo_children():
        if not w.winfo_ismapped():
            continue
        try:
            if isinstance(w, (ttk.Label, tk.Label)):
                t = str(w.cget("text")).strip()
                if t:
                    out.append(t)
            elif isinstance(w, (ttk.Radiobutton, ttk.Checkbutton)):
                on = w.instate(["selected"])
                if isinstance(w, ttk.Checkbutton):
                    mark = "[x] " if on else "[ ] "
                else:
                    mark = "(o) " if on else "( ) "
                out.append(mark + str(w.cget("text")) + (" (disabled)" if w.instate(["disabled"]) else ""))
            elif isinstance(w, ttk.Button):
                out.append("<button: %s%s>" % (w.cget("text"), ", disabled" if w.instate(["disabled"]) else ""))
            elif isinstance(w, ttk.Treeview):
                for iid in w.get_children():
                    vals = w.item(iid, "values")
                    out.append("  | " + " | ".join(str(v) for v in vals) + (" (selected)" if iid in w.selection()
                                                                            else ""))
            elif isinstance(w, tk.Text):
                body = w.get("1.0", "end").strip()
                if body:
                    lines = body.splitlines()
                    out.append("  [text, %d lines]\n    " % len(lines) + "\n    ".join(lines[-12:]))
            elif isinstance(w, ttk.Progressbar):
                out.append("<progress %s%%>" % int(float(w.cget("value"))))
            elif isinstance(w, ttk.Combobox):
                out.append("<choice: %s>" % w.get())
        except tk.TclError:
            pass
        if not isinstance(w, ttk.Progressbar):
            out.extend(text_of(w))
    return out


def default_font(root):
    # tkfont.nametofont takes a root only from Python 3.10; this is what it does with one.
    return tkfont.Font(root=root, name="TkDefaultFont", exists=True)


def wheel_units(event, platform):
    number = getattr(event, "num", None)
    if number in (4, 5):
        return -3 if number == 4 else 3
    delta = float(getattr(event, "delta", 0) or 0)
    if getattr(event, '_tk9_wheel', False):
        return -delta / 40
    return -delta if platform == 'darwin' else -delta / 120 * 3


def bind_touchpad(widget, tag, callback):
    # Tk 8.6 rejects the event name. Tk 9 precise devices never emit MouseWheel.
    if int(str(widget.tk.call('package', 'require', 'Tk')).split('.')[0]) >= 9:
        def precise(event):
            for axis, value in zip(('x', 'y'), touchpad_deltas(event.delta)):
                if value:
                    callback(types.SimpleNamespace(**dict(vars(event),
                        _scroll_axis=axis, _scroll_amount=-value)))
            return 'break'
        widget.bind_class(tag, '<TouchpadScroll>', precise)


def touchpad_deltas(delta):
    """Tk TIP 684: two signed 16-bit pixel deltas packed in %D."""
    value = int(delta)
    signed = lambda n: (n & 0xffff) - (0x10000 if n & 0x8000 else 0)
    return signed(value >> 16), signed(value)


class DetailsToggle:
    def __init__(self, parent, log):
        self.log = log
        self.open = False
        self.button = button(parent, text="Show details", command=self.toggle)
        self.button.pack(anchor="w")

    def show(self):
        if not self.open:
            self.toggle()

    def toggle(self):
        self.open = not self.open
        if self.open:
            self.log.pack(fill="both", expand=True)
        else:
            self.log.pack_forget()
        self.button.config(text="Hide details" if self.open else "Show details")


class Page:
    title = ""

    PAD_X, PAD_Y = 16, 14

    def __init__(self, app):
        self.app = app
        # box (the white border) > canvas (scrolls when the page is taller than the window) > frame (the content).
        self.box = tk.Frame(app.body, bg="#000000", highlightbackground="#ffffff", highlightcolor="#ffffff",
                            highlightthickness=3, bd=0)
        self.box._page_box = True
        self.box._ut_theme = app.theme
        self.canvas = tk.Canvas(self.box, bg="#000000", highlightthickness=0, bd=0, takefocus=0,
                                yscrollincrement=20, width=1, height=1)
        self.scroll = ttk.Scrollbar(self.box, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.scroll.set)
        self.canvas.pack(side="left", fill="both", expand=True)
        self.frame = tk.Frame(self.canvas, bg="#000000", highlightbackground="#ffffff", highlightcolor="#ffffff",
                              highlightthickness=0, bd=0, padx=self.PAD_X, pady=self.PAD_Y)
        self.frame._ut_theme = app.theme
        self.frame._page = self
        self._item = self.canvas.create_window(0, 0, window=self.frame, anchor="nw")
        self.wrappers = []
        self._width = 0
        self._fit_key = None
        self.canvas.bind("<Configure>", lambda _e: self.fit())
        self._wheel_tag = "BuilderPageWheel" + str(self.canvas)
        for sequence in ("<MouseWheel>", "<Shift-MouseWheel>", "<Button-4>", "<Button-5>",
                         "<Shift-Button-4>", "<Shift-Button-5>"):
            self.canvas.bind_class(self._wheel_tag, sequence, app._wheel)
        bind_touchpad(self.canvas, self._wheel_tag, app._wheel)

    def bind_wheel(self):
        # A page-specific tag runs before widget/class bindings, including on aqua.
        def visit(widget):
            tags = widget.bindtags()
            if self._wheel_tag not in tags:
                widget.bindtags((self._wheel_tag,) + tags)
            for child in widget.winfo_children():
                visit(child)
        visit(self.box)

    @property
    def st(self) -> State:
        return self.app.state

    def wrap_width(self, widget):
        """Usable text width for a widget on this page: the page width less the padding of the boxes around it."""
        if self._width <= 1:
            return 0
        inset = 2 * self.PAD_X + 4
        w = widget.master
        while w is not None and w is not self.frame:
            if isinstance(w, ttk.Labelframe):
                inset += 2 * 6 + 8
            elif isinstance(w, tk.Frame):
                inset += 2 * int(w.cget("padx") or 0)
            w = getattr(w, "master", None)
        return self._width - inset

    def fit(self):
        """Wrap text to the page width, give spare height to the content, and scroll only when it does not fit.

        Called on resize and from App.poll, since a page's content can grow without any resize event."""
        self.bind_wheel()
        c = self.canvas
        width, height = c.winfo_width(), c.winfo_height()
        if width <= 1 or height <= 1:
            return
        if width != self._width:
            self._width = width
            live = []
            for w in self.wrappers:
                if w.winfo_exists():
                    w.set_wrap(self.wrap_width(w))
                    live.append(w)
            self.wrappers = live
        need = self.frame.winfo_reqheight()
        key = (width, height, need)
        if key == self._fit_key:
            return
        self._fit_key = key
        c.itemconfigure(self._item, width=width, height=max(height, need))
        c.configure(scrollregion=(0, 0, width, max(height, need)))
        scrolls = need > height + 1
        if scrolls and not self.scroll.winfo_ismapped():
            self.scroll.pack(side="right", fill="y", before=c)
        elif not scrolls and self.scroll.winfo_ismapped():
            self.scroll.pack_forget()
        if not scrolls:
            c.yview_moveto(0)

    def scrollable(self):
        return bool(self.scroll.winfo_ismapped())

    def show_widget(self, widget):
        """Scroll so that a widget (just focused, or just shown) is in view."""
        if not self.scrollable():
            return
        top = widget.winfo_rooty() - self.frame.winfo_rooty()
        bottom = top + widget.winfo_height()
        view_top = self.canvas.canvasy(0)
        view_h = self.canvas.winfo_height()
        total = max(1, self.frame.winfo_height())
        if top < view_top:
            self.canvas.yview_moveto(max(0, top - 8) / total)
        elif bottom > view_top + view_h:
            self.canvas.yview_moveto(max(0, bottom + 8 - view_h) / total)

    def on_show(self):
        pass

    def on_next(self):
        return True

    def can_next(self):
        return True

    def busy(self):
        return False

    def auto(self):
        """Autopilot: 'next', 'wait', 'done' or ('fail', why)."""
        return "next"


class KeyWatch:
    """Which of Space and Enter are held down. One rule: a button, check box, choice or link acts on Space or Enter
    only for a new press, never for a held key's repeat. So a held key never carries into a button that appears under
    it (the safe choice of a prompt that just opened, whatever had the keyboard before a prompt closed, the next
    page's buttons), nor into one that a Tab or a click moves the keyboard to while the key is still down.

    Each control notes the key in its own binding before it acts (KeyWatch.note), since the press that closes a
    prompt destroys its button before the window's binding would see it. While a key is held after a prompt or page
    change, the window itself has the keyboard; on release it goes to the prompt's safe choice, or back."""

    KEYS = ("space", "Return", "KP_Enter")
    SETTLE_MS = 40      # X11 repeats a held key as a release straight away followed by a press

    def __init__(self, root):
        self.root = root
        self.held = set()
        self._releasing = {}
        self._pending = None
        self._waiting = False
        root._builder_keys = self
        root.bind("<KeyPress>", self.down, add="+")
        root.bind("<KeyRelease>", self.up, add="+")

    @staticmethod
    def note(event):
        """A control's own binding, ahead of its action: "break" for a held key's repeat, so the control never acts on
        it, whatever has the keyboard now."""
        root = getattr(event.widget, "_root", None)
        watch = getattr(root(), "_builder_keys", None) if root is not None else None
        if watch is not None and watch.press(event):
            return "break"
        return None

    def down(self, event):
        """The window's binding."""
        self.press(event)

    def press(self, event):
        """Note a key going down; True when it is a held key's repeat rather than a new press."""
        key = event.keysym
        if key not in self.KEYS:
            return False
        repeat = key in self.held
        token = self._releasing.pop(key, None)
        if token is not None:
            self.root.after_cancel(token)
        self.held.add(key)
        if repeat and self._waiting:
            self.root.focus_set()   # a Tab or a click while the key is held doesn't keep the keyboard
        return repeat

    def up(self, event):
        key = event.keysym
        if key in self.held and key not in self._releasing:
            self._releasing[key] = self.root.after(self.SETTLE_MS, lambda: self._released(key))

    def _released(self, key):
        # The only place a key stops counting as held: after a release this window saw (up), never inferred from
        # a dialog closing or the window losing focus. A release that went elsewhere costs one later press, which
        # then does nothing until its own release; never an action.
        self._releasing.pop(key, None)
        self.held.discard(key)
        if not self.held and self._waiting:
            self._settle()

    def hand(self, widget):
        """Give `widget` (None: the window) the keyboard now, or once no key is held."""
        self._pending, self._waiting = widget, True
        if self.held:
            self.root.focus_set()
        else:
            self._settle()

    def carry(self):
        """A page changed: if a key is still held, the window keeps the keyboard until it is released."""
        if self.held and not self._waiting:
            self.hand(self.root.focus_lastfor())

    def _settle(self):
        widget, self._pending, self._waiting = self._pending, None, False
        try:
            self.root.update_idletasks()    # a just-packed prompt is laid out, so it counts as shown
            # Never a button on a page that is no longer shown (the window itself is shown in normal use).
            usable = (widget is not None and widget.winfo_exists() and
                      (widget.winfo_viewable() or not self.root.winfo_viewable()) and
                      (not hasattr(widget, "instate") or widget.instate(["!disabled"])))
            (widget if usable else self.root).focus_set()
        except tk.TclError:
            pass


class Prompt(Page):
    """A question, a notice or a file list shown inside the main window, in place of the page, with the builder's
    own look: no separate, unthemed window. App.prompt opens one; Back and Next stay off until a choice closes it.

    choices: (text, callback or None, primary) from left to right. cancel: the choice that Escape and the window's
    close button make. focus: the choice that has the keyboard first (the safe one for anything that deletes or
    stops)."""

    def __init__(self, app, title, message, choices, details=None, cancel=None, focus=None, collapse_details=False):
        super().__init__(app)
        self.title, self.message, self.details = title, message, details
        self.cancel = cancel if cancel is not None else choices[0][0]
        self.closed = False
        label(self.frame, message)
        self.list = None
        if details is not None:
            area = tk.Frame(self.frame, bd=0)
            self.details_area = area
            self.list = tk.Text(area, wrap='word' if collapse_details else 'none', height=8, width=60,
                                takefocus=1, font=app.theme.mono_font,
                                bg="#000000", fg="#ffffff", insertbackground="#ffffff",
                                highlightbackground="#ffffff", highlightcolor="#ffffff", highlightthickness=2, bd=0)
            down = ttk.Scrollbar(area, orient="vertical", command=self.list.yview)
            across = ttk.Scrollbar(area, orient="horizontal", command=self.list.xview)
            self.list.configure(yscrollcommand=down.set, xscrollcommand=across.set)
            down.pack(side="right", fill="y")
            across.pack(side="bottom", fill="x")
            self.list.pack(side="left", fill="both", expand=True)
            self.list.insert("1.0", details)
            self.list.configure(state="disabled")
            if collapse_details:
                def toggle_details():
                    opened = bool(area.winfo_manager())
                    if opened:
                        area.pack_forget()
                    else:
                        area.pack(fill='both', expand=True, pady=(0, 8), after=self.details_toggle)
                    self.details_toggle.config(text='Show all files' if opened else 'Hide file list')
                self.details_toggle = button(self.frame, text='Show all files', command=toggle_details)
                self.details_toggle.pack(anchor='w', pady=(0, 8))
            else:
                area.pack(fill="both", expand=True, pady=(0, 8))
        row = ttk.Frame(self.frame)
        row.pack(anchor="e", pady=(4, 0))
        self.choices, self.buttons = {}, {}
        for text, callback, primary in reversed(choices):
            widget = button(row, text=text, command=lambda t=text: self.choose(t), primary=primary)
            widget.pack(side="right", padx=(8, 0))
            for seq in ("<Return>", "<KP_Enter>"):
                widget.bind(seq, lambda e, t=text: self._key(e, t))
            self.choices[text], self.buttons[text] = callback, widget
        self.first = self.buttons[focus if focus is not None else choices[-1][0]]

    def _key(self, event, text):
        if KeyWatch.note(event) == "break":     # a held Enter's repeat chooses nothing
            return "break"
        self.choose(text)   # noted before the button goes, so the held key cannot choose what comes next
        return "break"

    def choose(self, text):
        """Close the prompt, show the page again, then run the choice's callback (which may open another)."""
        if self.closed:
            return
        self.closed = True
        callback = self.choices[text]
        self.app.close_prompt(self)
        if callback is not None:
            callback()


# ---------------------------------------------------------------------------------------------------------------------

class WelcomePage(Page):
    title = "Welcome"

    def __init__(self, app):
        super().__init__(app)
        label(self.frame, DISCLAIMER, bold=True)
        label(self.frame, WHAT)
        try:
            release = "Release %s. Old and New 3DS families. Aims for a steady 30 fps." % pipeline.release_version()
        except pipeline.BuildFailed:
            release = "Source files need repair. Download and unpack the builder again."
        label(self.frame, release)
        label(self.frame, TERMS)
        self.check_updates = tk.BooleanVar(app.root, value=preferences.load(app.opts.work).get('updates', True))
        checkbutton(self.frame, text='Allow update checks (GitHub)', variable=self.check_updates,
                    command=self.update_preference).pack(anchor='w')
        self.update_button = button(self.frame, text='Check for updates', command=lambda: app.check_updates(manual=True))
        self.update_button.pack(anchor='w')
        self.update_button.state(['!disabled'] if self.check_updates.get() else ['disabled'])
        self.update_status = label(self.frame, '')
        self.update_feedback = label(self.frame, '')
        self.release_button = button(self.frame, text='Open release page', command=self.open_release)
        self.release_url = None
        self.game = tk.StringVar(value=app.state.game)
        if not app.opts.game:
            label(self.frame, "Game:", bold=True)
            for key, p in products.PRODUCTS.items():
                radiobutton(self.frame, text=p.title, value=key, variable=self.game,
                            command=self.changed).pack(anchor="w")
        self.cleanup_button = button(self.frame, text="Clean up previous build files",
                                     command=self.app.open_cleanup)

    def on_show(self):
        if cleanup.available(self.app.opts.work):
            self.cleanup_button.pack(anchor="w", pady=(12, 0))
        else:
            self.cleanup_button.pack_forget()

    def changed(self):
        if self.game.get() != self.st.game:
            self.app.reset(self.game.get())

    def on_next(self):
        if self.app.opts.autopilot is None:
            self.app.check_updates()
        return True

    def update_preference(self):
        preferences.save(self.app.opts.work, self.app.theme.boring, updates=self.check_updates.get())
        self.update_button.state(['!disabled'] if self.check_updates.get() else ['disabled'])
        if self.check_updates.get():
            self.app.check_updates()
        else:
            self.update_status.config(text='')
            self.update_feedback.config(text='')
            self.release_button.pack_forget()
            self.release_url = None

    def open_release(self):
        if self.release_url:
            webbrowser.open(self.release_url)

    def release_notice(self, notice):
        if notice is None:
            return
        self.release_url = notice.url if notice else None
        self.update_status.config(text=('v%s is out\n%s\nRebuild and deliver to update. Your saves are kept.'
                                       % (notice.version, notice.notes)) if notice else '')
        if notice:
            self.release_button.pack(anchor='w')
        else:
            self.release_button.pack_forget()

    def auto(self):
        want = (self.app.opts.autopilot or {}).get("game")
        if want and want != self.st.game:
            self.game.set(want)
            self.changed()
        return "next"


class FindPage(Page):
    title = "Find the game"

    def __init__(self, app):
        super().__init__(app)
        self.intro = label(self.frame, "")
        self.tree = ttk.Treeview(self.frame, columns=("store", "folder", "version"), show="headings", height=6,
                                 selectmode="browse")
        for c, w in (("store", 80), ("folder", 260), ("version", 200)):
            self.tree.heading(c, text="Found in" if c == "store" else c.capitalize())
            self.tree.column(c, width=w, minwidth=60, stretch=(c == "folder"))
        self.tree.pack(fill="x", pady=(0, 8))
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.picked())
        row = ttk.Frame(self.frame)
        row.pack(anchor="w")
        button(row, text="Search again", command=self.search).pack(side="left", padx=(0, 8))
        button(row, text="Choose folder...", command=self.choose).pack(side="left")
        self.status = label(self.frame, "")
        self.searching = False
        self.searched_for = None
        # Each search has a number. Choosing a folder by hand bumps it, so a search still running then can't
        # replace that choice when it finishes. Folders chosen by hand stay listed after later searches.
        self.search_generation = 0
        self.manual = []

    def on_show(self):
        self.intro.config(text=("Choose your UNDERTALE files: Windows Steam, macOS Steam or Windows GOG.\n"
                                "On Linux, use the Windows files. For Mac GOG, use the Windows GOG files.")
                          if self.st.product.key == 'undertale' else "Choose your DELTARUNE game folder.")
        if self.searched_for != self.st.game:
            if self.app.opts.folder:
                self.searched_for = self.st.game
                self.add_manual(self.app.opts.folder)
            else:
                self.search()

    def forget_check(self):
        """The game may have been repaired or replaced since its last check, so check it again (SCOURSOL4-002)."""
        if self.st.verdict is not None:
            self.st.verdict = None
            self.st.inputs += 1
            self.st.invalidate_build()

    def search(self):
        self.forget_check()
        self.searching = True
        self.searched_for = self.st.game
        self.tree.delete(*self.tree.get_children())
        self.status.config(text="Searching...")
        game = self.st.game
        self.search_generation += 1
        generation = self.search_generation
        self.app.work(lambda: discover.find_copies(game, self.app.opts.probes),
                      lambda copies: self.found(copies, generation))

    def found(self, copies, generation=None):
        if generation is not None and generation != self.search_generation:
            return  # The player chose a folder (or searched again) since this search started.
        self.searching = False
        chosen = self.st.folder
        self.manual = list(dict.fromkeys(products.verify_game.install_folder(self.st.product.game, d)
                                         for d in self.manual))
        self.tree.delete(*self.tree.get_children())
        for i, c in enumerate(copies):
            self.tree.insert("", "end", iid=str(i), values=(c.store, c.folder, c.version + (" - " + c.note if c.note
                                                                                             else "")))
        listed = [c.folder for c in copies]
        for d in self.manual:
            if d not in listed:
                self.tree.insert("", "end", iid=str(len(listed)), values=("folder", d, "-"))
                listed.append(d)
        if listed:
            self.tree.selection_set(str(listed.index(chosen)) if chosen in listed else "0")
        if copies:
            self.status.config(text="Found %d %s." % (len(copies), "copy" if len(copies) == 1 else "copies"))
        else:
            p = self.st.product
            self.status.config(text="%s was not found. Choose folder..., or install the game and Search again."
                                    % p.title)
        self.picked()

    def picked(self):
        sel = self.tree.selection()
        if sel:
            new = self.tree.item(sel[0], "values")[1]
            if new != self.st.folder:
                self.st.folder = new
                self.st.verdict = None
                self.st.inputs += 1
                self.st.invalidate_build()
                self.app.theme.load_from_game_dir(new)
        self.app.update_nav()

    def choose(self):
        d = filedialog.askdirectory(title="Select %s folder" % self.st.product.title)
        if d:
            self.add_manual(os.path.normpath(d))

    def add_manual(self, d):
        self.forget_check()  # Choosing the same folder again checks it again.
        self.search_generation += 1  # A search still running must not replace this choice.
        self.searching = False
        if d not in self.manual:
            self.manual.append(d)
        iid = str(len(self.tree.get_children()))
        self.tree.insert("", "end", iid=iid, values=("folder", d, "-"))
        self.tree.selection_set(iid)
        self.picked()

    def can_next(self):
        return bool(self.tree.selection()) and not self.searching

    def busy(self):
        return self.searching

    def auto(self):
        if self.searching:
            return "wait"
        want = (self.app.opts.autopilot or {}).get("folder")
        if want and os.path.normcase(os.path.normpath(want)) != os.path.normcase(os.path.normpath(self.st.folder or
                                                                                                   "")):
            self.add_manual(os.path.normpath(want))
        return "next" if self.can_next() else ("fail", self.status.cget("text"))


class VerifyPage(Page):
    title = "Check the game"

    def __init__(self, app):
        super().__init__(app)
        self.intro = label(self.frame, "")
        self.bar = progressbar(self.frame, mode="indeterminate", length=500)
        self.bar.pack(fill="x", pady=(0, 8))
        self.result = label(self.frame, "")
        self.details = label(self.frame, "")
        self.links = ttk.Frame(self.frame)
        self.links.pack(anchor="w")
        self.running = False

    def on_show(self):
        # A refusal asks the player to repair the game and try again, so coming back here checks it again.
        if (self.st.verdict is None or not self.st.verdict.accepted) and not self.running:
            self.start()
        else:
            self.render()

    def start(self):
        self.running = True
        self.intro.config(text="Checking game files...")
        self.result.config(text="Checking game files...")
        self.details.config(text="")
        for w in self.links.winfo_children():
            w.destroy()
        self.bar.start(12)
        game, folder = self.st.game, self.st.folder
        self.app.work(lambda: discover.verify_copy(game, folder, self.app.opts.probes), self.done)

    def done(self, verdict):
        self.running = False
        self.bar.stop()
        self.st.verdict = verdict
        self.st.inputs += 1
        self.st.invalidate_build()
        if verdict.accepted:
            self.st.folder = verdict.result.folder
            self.st.parts = [p for p in verdict.parts if p != "root"] or None
        self.render()
        self.app.update_nav()

    def render(self):
        if self.running:
            self.intro.config(text="Checking game files...")
            return
        v = self.st.verdict
        if v is None:
            return
        self.intro.config(text="Game files checked." if v.accepted else "Game files not accepted.")
        self.bar.config(mode="determinate", value=100)
        self.result.config(text=("OK: " if v.accepted else "Refused: ") + v.message)
        self.details.config(text="")  # Raw paths and OS errors stay on the result for support.
        for w in self.links.winfo_children():
            w.destroy()
        for url in v.store_links:
            button(self.links, text="Open " + url.split("/")[2], command=lambda u=url: webbrowser.open(u)).pack(
                side="left", padx=(0, 8))

    def can_next(self):
        return bool(self.st.verdict and self.st.verdict.accepted)

    def busy(self):
        return self.running

    def auto(self):
        if self.running or self.st.verdict is None:
            return "wait"
        return "next" if self.st.verdict.accepted else ("fail", self.st.verdict.message)


class StepBox:
    """One setup step: what it does, one button that does it, its own progress, and its own result.

    A failed step keeps its "Technical details" and "Open download page" buttons for support and setup information."""

    def __init__(self, page, parent, step, button_text="Install"):
        self.page, self.step = page, step
        dry = page.app.opts.toolchain_dry_run
        optional = step.key in ('makerom', 'bannertool')
        self.box = ttk.LabelFrame(parent, text=('Optional: ' if optional else '') + step.title, padding=6)
        self.box.pack(fill="x", pady=(0, 6))
        # Full consent remains in the confirmation and the adjacent Licences dialog.
        self.row = ttk.Frame(self.box)
        self.row.pack(fill="x")
        self.go = button(self.row, text="Technical details" if dry else button_text,
                         command=lambda: page.run_step(self), primary=not optional)
        self.go.pack(side="left", padx=(0, 8))
        self.show_btn = button(self.row, text="Technical details", command=self.show_command)
        self.page_btn = (button(self.row, text="Open download page", command=lambda: webbrowser.open(step.page))
                         if step.page else None)
        self.bar = progressbar(self.box, mode="determinate", length=400, maximum=100)
        self.status = label(self.box, "")
        self.status.pack_forget()
        if step.key in page.failures:
            self.failed(page.failures[step.key])

    def _say(self, text):
        self.status.config(text=text)
        if text and not self.status.winfo_manager():
            self.status.pack(anchor="w", pady=(4, 0))

    def start(self):
        self.go.state(["disabled"])
        for w in (self.show_btn, self.page_btn):
            if w is not None:
                w.pack_forget()
        self.bar.pack(fill="x", pady=(6, 0), after=self.row)
        self.bar.config(mode="indeterminate")
        self.bar.start(12)
        self._say("Starting...")

    def stage(self, kind, text):
        self._say(text)
        self.bar.stop()
        if kind == "download":
            self.bar.config(mode="determinate", value=0)
        else:
            self.bar.config(mode="indeterminate")
            self.bar.start(12)

    def progress(self, done, total):
        if total:
            self.bar.config(mode="determinate", value=100.0 * done / total)

    def succeeded(self):
        self.bar.stop()
        self.bar.config(mode="determinate", value=100)
        self.go.state(["!disabled"])
        self._say("Done.")

    def failed(self, why):
        self.bar.stop()
        self.bar.pack_forget()
        self.go.state(["!disabled"])
        if why.startswith(("Couldn't check", "The 3DS compiler wasn't found in ",
                           "The devkitPro installer finished, but its MSYS2 tools couldn't be found.")):
            self.go.config(text="Try again")
        self.show_btn.pack(side="left", padx=(0, 8))
        if self.page_btn is not None:
            self.page_btn.pack(side="left")
        self._say("Failed: %s" % why)

    def show_command(self):
        self.page.write(self.step.describe())
        self.page.show_log()


class ToolchainPage(Page):
    title = "Set up the 3DS compiler"

    def __init__(self, app):
        super().__init__(app)
        mode = " (Dry run: the buttons only show what they would do.)" if app.opts.toolchain_dry_run else ""
        label(self.frame, "Checks for devkitPro and the other build tools." + mode)
        self.tree = ttk.Treeview(self.frame, columns=("ok", "item", "detail"), show="headings", height=3,
                                 selectmode="none")
        for c, w in (("ok", 110), ("item", 250), ("detail", 220)):
            self.tree.heading(c, text={"ok": "", "item": "Item", "detail": "Notes"}[c])
            self.tree.column(c, width=w, minwidth=60, stretch=(c == "detail"))
        self.tree.pack(fill="x", pady=(0, 8))
        self.tree.bind('<<ThemeChanged>>', lambda _e: self.fit_status())
        self.fit_status()
        self.result = label(self.frame, "")
        self.compiler_status = label(self.frame, "")
        self.steps = ttk.Frame(self.frame)
        self.steps.pack(fill="x")
        self.extra = ttk.Frame(self.frame)
        self.extra.pack(fill="x")
        notice, links = toolchain.licence_notice(sys.platform)
        self.notice = label(self.frame, "Install or Update means you agree to the listed licences.")
        self.notice.pack_configure(before=self.steps)
        row = ttk.Frame(self.frame)
        row.pack(anchor="w", pady=(0, 8), before=self.steps)
        button(row, text="Licences", command=lambda: self.app.inform(
            "Licences", notice, details="\n\n".join(name + "\n" + url for name, url in links))).pack(side="left")
        row = ttk.Frame(self.frame)
        row.pack(anchor="w", pady=(0, 8))
        button(row, text="Check again", command=self.detect).pack(side="left", padx=(0, 8))
        self.update_btn = button(row, text="Update devkitPro", command=self.update)
        self.update_btn.pack(side="left", padx=(0, 8))
        if app.opts.toolchain_dry_run:
            button(row, text="Dry run", command=self.show_dry_run).pack(side="left")
        self.log = tk.Text(self.frame, height=6, width=60, wrap="word", font=app.theme.mono_font,
                           bg="#000000", fg="#ffffff", insertbackground="#ffffff",
                           highlightbackground="#ffffff", highlightcolor="#ffffff", highlightthickness=2, bd=0)
        self.details = DetailsToggle(self.frame, self.log)
        self.detecting = False
        self.running_step = False
        self.dry_run_shown = False
        self.failures = {}
        self.update_box = None

    def on_show(self):
        if self.st.tc is None and not self.detecting:
            self.detect()

    def write(self, s):
        self.log.insert("end", s.rstrip("\n") + "\n")
        self.log.see("end")

    def show_log(self):
        self.details.show()

    def detect(self):
        self.detecting = True
        self.tree.delete(*self.tree.get_children())
        opts = self.app.opts
        roots = [opts.devkitpro] if opts.devkitpro else None
        self.app.update_nav()
        def job():
            tc = toolchain.detect(opts.tools_dir, roots=roots)
            tc.compiler_info = toolchain.compiler_details(tc)
            toolchain.check_build_tools(tc)
            return tc
        self.app.work(job, self.detected)

    def detected(self, tc):
        self.detecting = False
        if isinstance(tc, Exception):
            self.result.config(text="Could not check the tools: %s" % tc)
            self.write(str(tc))
            self.show_log()
            self.app.update_nav()
            return
        self.st.tc = tc
        tested = products.release_contract.read(pipeline.SOURCE_ROOT)['tested_compiler']
        self.write(toolchain.compiler_status(tc.compiler_info, tested))
        text = "The 3DS tools are listed below. Use Install for anything missing."
        if self.st.game == 'undertale':
            text += '\nInstall and Update check the offered compiler before changing devkitPro.'
        self.compiler_status.config(text=text)
        # Remember what existed before any installer button is pressed.
        try:
            cleanup.record_before(self.app.opts.work, [p for p in (tc.root, tc.makerom, tc.bannertool) if p])
        except (OSError, ValueError) as exc:
            self.result.config(text=str(exc))
        steps = toolchain.full_plan(self.app.opts.tools_dir, self.app.opts.downloads_dir,
                                   root=tc.root, platform=tc.platform,
                                   want_cia=toolchain.cia_download_supported(tc.platform))
        if not getattr(self, 'boxes', None):
            self.boxes = [StepBox(self, self.steps, s) for s in steps]
        for box, step in zip(self.boxes, steps):
            box.step = step
        self.tree.delete(*self.tree.get_children())
        self.tree.configure(height=min(8, max(1, len(steps))))
        cleared_failure = False
        inventory = toolchain.setup_inventory(tc, steps)
        next_step = next((step for step, needed, waiting in inventory
                          if needed and not waiting and step.key not in ('makerom', 'bannertool')), None)
        if tc.can_build:
            guidance = ('Choose Next to continue. You can also choose Install beside "%s" for additional tools.'
                        % next_step.title if next_step else
                        'Required tools are installed. Choose Next to continue; HOME Menu tools are optional.')
        else:
            guidance = ('Choose Install beside "%s". Next stays grey until the required tools are installed.'
                        % next_step.title if next_step else
                        'Next stays grey until the required tools are installed. Check the steps below.')
        broken = toolchain.broken_tools_text(tc.checks, tc.root) if isinstance(tc.checks, list) else ''
        if broken:
            # Installed but unable to start: no Install box covers it, so say what is wrong and the fix.
            guidance = broken
            for check in tc.checks:
                if check.technical:
                    self.write(check.technical)
            self.show_log()
        self.compiler_status.config(text=guidance)
        for i, (step, needed, waiting) in enumerate(inventory):
            missing = 'Not installed' if step.key in ('devkitpro', 'makerom', 'bannertool') else 'Needs setup'
            status = "Installed" if not needed else "Waiting for %s." % waiting if waiting else missing
            # Full titles and wrapping dependency explanations remain below the table. Keep its cells compact.
            item = {'devkitpro': 'devkitPro', 'packages': '3DS tools', 'host': 'Computer tools',
                    'clt': 'Apple build tools', 'cmake': 'CMake', 'makerom': 'CIA builder',
                    'bannertool': 'CIA banner tool'}.get(step.key, step.title)
            self.tree.insert("", "end", iid=str(i), values=("Waiting" if needed and waiting else status, item,
                             "Optional" if step.key in ('makerom', 'bannertool') else ""))
            box = self.boxes[i]
            if needed and step.key in self.failures:
                box.failed(self.failures[step.key])
            else:
                if self.failures.pop(step.key, None) is not None:
                    cleared_failure = True
                    box.go.config(text="Technical details" if self.app.opts.toolchain_dry_run else "Install")
                    for widget in (box.show_btn, box.page_btn):
                        if widget is not None:
                            widget.pack_forget()
                box._say(status)
        if cleared_failure:
            self.result.config(text=next(iter(self.failures.values())) if self.failures else "Tools checked.")
        self.fit_status()
        self.update_buttons()
        self.app.update_nav()

    def fit_status(self):
        # Theme/font changes can arrive before detection has populated the rows.
        font = tkfont.Font(self.tree, font=ttk.Style(self.tree).lookup('Treeview', 'font') or 'TkDefaultFont')
        rows = [self.tree.item(row, 'values') for row in self.tree.get_children()]
        for index, (column, fallback) in enumerate((('ok', 'Installed'), ('item', 'Item'), ('detail', 'Notes'))):
            labels = [fallback] + [row[index] for row in rows]
            width = max(60, max(font.measure(text) for text in labels) + 28)
            self.tree.column(column, width=width, minwidth=width, stretch=(column == 'detail'))

    def update_buttons(self):
        busy = self.running_step or self.detecting
        ok = bool(self.st.tc and self.st.tc.root) and not busy
        self.update_btn.state(["!disabled"] if ok else ["disabled"])
        boxes = getattr(self, "boxes", [])
        inventory = toolchain.setup_inventory(self.st.tc, [box.step for box in boxes]) if self.st.tc else []
        for box, (_, needed, waiting) in zip(boxes, inventory):
            if box.box.winfo_exists():
                box.go.state(["disabled"] if busy or not needed or waiting else ["!disabled"])

    def show_dry_run(self):
        tc = self.st.tc
        if tc is None:
            return
        report = toolchain.dry_run_report(tc, self.app.opts.tools_dir, self.app.opts.downloads_dir)
        self.write(report)
        self.show_log()
        self.dry_run_shown = True
        if self.app.opts.autopilot is not None:
            print(report, flush=True)

    def run_step(self, box, confirmed=False):
        """True when the step starts, False when it doesn't, None while the window asks first (its OK starts it)."""
        step = box.step
        dry = self.app.opts.toolchain_dry_run
        if self.running_step:
            return False
        tested = products.release_contract.read(pipeline.SOURCE_ROOT)['tested_compiler']
        releases_url = updates.repository(self.st.game) + '/releases'
        if not dry and not confirmed:
            ask = self.app.opts.confirm_install
            if ask is None:
                self.app.ask(step.title, step.confirm_text(), lambda: self.run_step(box, confirmed=True) or
                             self.declined(box), lambda: self.declined(box), yes_text="OK", no_text="Cancel",
                             safe=False)
                return None
            if not ask(step.title, step.confirm_text()):
                return False
        self.running_step = True
        self.failures.pop(step.key, None)
        self.result.config(text="")
        self.update_buttons()
        self.app.update_nav()
        box.start()
        log = lambda s: self.app.call(self.write, s)  # noqa: E731
        progress = lambda done, total: self.app.call(box.progress, done, total)  # noqa: E731
        actions = toolchain.Actions(
            run=lambda argv, lg: stream(argv, lg),
            download=lambda url, dest, sha, size=None: toolchain.download(url, dest, sha, progress=progress,
                                                                          size=size))
        game = self.st.game
        root = self.st.tc.root
        notices = []

        def stage(kind, text):
            if kind == 'notice':
                notices.append(text)
            self.app.call(box.stage, kind, text)

        def job():
            toolchain.run_step(step, log, dry, actions, stage=stage,
                               work=self.app.opts.work, root=root, game=game,
                               tested=tested, releases_url=releases_url)
            return step

        def after(result):
            self.running_step = False
            if isinstance(result, Exception):
                why = str(result) if isinstance(result, toolchain.Refused) else "%s: %s" % (type(result).__name__,
                                                                                         result)
                self.failures[step.key] = why
                self.write("Stopped: %s" % why)
                box.failed(why)
                self.show_log()
                self.result.config(text=(why + '\n' if isinstance(result, toolchain.Refused) else
                                         '%s did not finish. ' % step.title) +
                                   'Technical details are available for support.')
            else:
                box.succeeded()
                if notices:
                    box._say(notices[-1])
                if not dry:
                    self.result.config(text=notices[-1] if notices else "Done: %s." % step.title)
            if not dry and not isinstance(result, Exception):
                if step.key == 'update' and box is self.update_box:
                    box.box.destroy()
                    self.update_box = None
                self.detect()
            self.update_buttons()
            self.app.update_nav()
        self.app.work(job, after, errors=True)
        return True

    def update(self):
        if self.update_box is not None and self.update_box.box.winfo_exists():
            self.update_box.box.destroy()
        step = toolchain.update_step(self.st.tc.root if self.st.tc else None)
        self.update_box = StepBox(self, self.extra, step, button_text="Update")
        if self.run_step(self.update_box) is False:
            self.declined(self.update_box)

    def declined(self, box):
        """A step that did not start: an Update box made just for it goes away again."""
        if box is self.update_box and box.box.winfo_exists():
            box.box.destroy()
            self.update_box = None

    def can_next(self):
        return bool(self.st.tc and self.st.tc.can_build) and not self.running_step

    def busy(self):
        return self.detecting or self.running_step

    def auto(self):
        if self.detecting or self.st.tc is None:
            return "wait"
        if self.app.opts.toolchain_dry_run and not self.dry_run_shown:
            self.show_dry_run()
        return "next" if self.can_next() else ("fail", "toolchain incomplete")


def stream(argv, log):
    p = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         errors="replace", **hidden_options())
    for line in p.stdout:
        log(line)
    return p.wait()


class OptionsPage(Page):
    title = "Options"

    def __init__(self, app):
        super().__init__(app)
        label(self.frame, "Console", bold=True)
        self.target = tk.StringVar(value="both")
        radiobutton(self.frame, text="Both New 3DS and Old 3DS (recommended)",
                    value="both", variable=self.target, command=self.changed).pack(anchor="w")
        for key, name in products.TARGETS.items():
            radiobutton(self.frame, text=name, value=key, variable=self.target,
                        command=self.changed).pack(anchor="w")
        label(self.frame, products.TARGET_HINT)
        label(self.frame, "Format", bold=True)
        self.v3dsx = tk.BooleanVar(value=True)
        self.vcia = tk.BooleanVar(value=False)
        checkbutton(self.frame, text=".3dsx (Homebrew Launcher)", variable=self.v3dsx,
                    command=self.changed).pack(anchor="w")
        self.cia_box = checkbutton(self.frame, text=".cia (HOME Menu, needs custom firmware and FBI, the 3DS installer)",
                                   variable=self.vcia, command=self.changed)
        self.cia_box.pack(anchor="w")
        self.cia_note = label(self.frame, "")
        self.extra = ttk.Frame(self.frame)
        self.extra.pack(fill="x")
        self.estimate = label(self.frame, "")
        self.part_vars = {}

    def on_show(self):
        tc = self.st.tc
        if tc and tc.can_cia:
            self.cia_box.state(["!disabled"])
            self.cia_note.config(text="")
        else:
            self.vcia.set(False)
            self.cia_box.state(["disabled"])
            self.cia_note.config(text=("Automatic HOME Menu tool downloads are not available for this computer. "
                                      "Use the Homebrew Launcher build, or install the tools yourself and check again."
                                      if tc and not toolchain.cia_download_supported(tc.platform) else
                                      "To make a HOME Menu install file, go Back and install makerom and bannertool."))
        for w in self.extra.winfo_children():
            w.destroy()
        self.part_vars = {}
        if self.st.game == "deltarune" and self.st.verdict:
            import chapters
            states = chapters.inventory(self.st.folder)
            installed = [p for p, v in states.items() if p != 'root' and v[0] != 'missing']
            label(self.extra, 'Version: %s\nInstalled chapters: %s' %
                  (self.st.verdict.result.version or 'unknown', chapters.numbers(installed)), bold=True)
            # Coming back keeps the player's choices: st.parts starts as every checked chapter (VerifyPage) and
            # then holds the ticks, even none.
            chosen = self.st.parts
            for part in self.st.verdict.parts:
                if part == "root":
                    continue
                v = tk.BooleanVar(value=chosen is None or part in chosen)
                self.part_vars[part] = v
                checkbutton(self.extra, text="Chapter " + part[len("chapter")], variable=v,
                            command=self.changed).pack(anchor="w")
            self.h264 = tk.BooleanVar(value=bool(self.st.openh264))
            checkbutton(self.extra, text="Download openh264 2.6.0 for intro videos",
                        variable=self.h264, command=self.changed).pack(anchor="w")
        self.changed()

    def changed(self):
        previous = (tuple(self.st.targets), tuple(self.st.outputs), tuple(self.st.parts or ()), self.st.openh264)
        t = self.target.get()
        self.st.targets = ["n3ds", "o3ds"] if t == "both" else [t]
        self.st.outputs = [k for k, v in (("3dsx", self.v3dsx), ("cia", self.vcia)) if v.get()]
        if self.part_vars:
            self.st.parts = [p for p, v in self.part_vars.items() if v.get()]
        self.st.openh264 = bool(getattr(self, "h264", None) and self.h264.get())
        p = self.st.product
        disk = p.work_disk_mb(self.st.targets)
        if self.st.game == 'deltarune' and self.st.folder:
            import chapters
            disk += (chapters.stage_bytes(self.st.folder, self.st.parts) + (1 << 20) - 1) // (1 << 20)
        if self.st.game == 'undertale':
            self.estimate.config(text=("Allow about %d MB in %s for source, builds and caches.\n"
                                       "Delivery copies need extra space where you save them.") %
                                      (disk, self.app.opts.work))
        else:
            self.estimate.config(text="About %d MB in %s." % (disk, self.app.opts.work))
        if self.st.game == 'deltarune':
            import chapters
            self.estimate.config(text=self.estimate.cget('text') + '\nSelected chapters: ' +
                                 chapters.numbers(self.st.parts or []))
        selected = (tuple(self.st.targets), tuple(self.st.outputs), tuple(self.st.parts or ()), self.st.openh264)
        if selected != previous:
            self.st.invalidate_build()
        self.app.update_nav()

    def can_next(self):
        return bool(self.st.outputs and (self.st.game != 'deltarune' or self.st.parts))

    def auto(self):
        ap = self.app.opts.autopilot or {}
        targets = ap.get("targets") or ["n3ds", "o3ds"]
        self.target.set("both" if len(targets) > 1 else targets[0])
        outs = ap.get("outputs") or ["3dsx"]
        self.v3dsx.set("3dsx" in outs)
        self.vcia.set("cia" in outs and not self.cia_box.instate(["disabled"]))
        if 'chapters' in ap:
            chosen = {str(n) for n in ap['chapters']}
            if not chosen or any(n not in '12345' or len(n) != 1 for n in chosen):
                raise ValueError('Choose chapters from 1 to 5.')
            available = {part[len('chapter')] for part in self.part_vars}
            if not chosen <= available:
                raise ValueError('A selected chapter is unavailable.')
            for part, var in self.part_vars.items():
                var.set(part[len('chapter')] in chosen)
        if hasattr(self, 'h264'):
            self.h264.set(bool(ap.get('openh264', False)))
        self.changed()
        return "next"


CANCELLED = "The build was cancelled.\nClick Try again when ready."  # Build.run's own text.


class BuildPage(Page):
    title = "Build"

    def __init__(self, app):
        super().__init__(app)
        self.intro = label(self.frame, "", bold=True)
        self.bar = progressbar(self.frame, mode="determinate", length=560, maximum=100)
        self.bar.pack(fill="x", pady=(0, 6))
        self.eta = label(self.frame, "")
        self.packaging = progressbar(self.frame, mode='indeterminate', length=180)
        self.status = label(self.frame, "")
        row = ttk.Frame(self.frame)
        row.pack(anchor="w", pady=(0, 6))
        self.cancel_btn = button(row, text="Cancel", command=self.cancel)
        self.cancel_btn.pack(side="left", padx=(0, 8))
        self.retry_btn = button(row, text="Try again", command=self.start)
        self.retry_btn.pack(side="left", padx=(0, 8))
        self.copy_btn = button(row, text="Copy log", command=self.copy_log)
        self.copy_btn.pack(side="left", padx=(0, 8))
        button(row, text="Work folder", command=lambda: open_folder(self.app.opts.work)).pack(side="left")
        self.log = tk.Text(self.frame, height=12, width=86, wrap="none", font=app.theme.mono_font,
                           bg="#000000", fg="#ffffff", insertbackground="#ffffff",
                           highlightbackground="#ffffff", highlightcolor="#ffffff", highlightthickness=2, bd=0)
        self.details = DetailsToggle(self.frame, self.log)
        self.build = None
        self.running = False
        self.failed = None
        self.log_path = None
        self.target_index = 0
        self.input_key = None
        # A confirmed Cancel stops the whole run, including a model whose build has not started yet (AUD-14).
        self.stop = threading.Event()
        self.worker_done = threading.Event()    # no worker is running: quitting waits on it (TK-D9)
        self.worker_done.set()

    def on_show(self):
        if self.st.build_ok and self.st.build_key is not None and self.st.build_key != self.st.build_input_key():
            self.st.invalidate_build()
        self.intro.config(text="Building..." if self.running else
                         "Build complete." if self.st.build_ok else "Build stopped.")
        if not self.st.build_ok and not self.running:
            self.start()
        self.buttons()

    def buttons(self):
        self.cancel_btn.state(["!disabled"] if self.running else ["disabled"])
        game_files = isinstance(self.failed, pipeline.GameFilesRefused)
        self.retry_btn.config(text="Choose game files" if game_files else "Try again",
                              command=self.choose_game_files if game_files else self.start)
        self.retry_btn.state(["!disabled"] if (self.failed and not self.running) else ["disabled"])
        self.copy_btn.state(["!disabled"] if self.log_path and os.path.isfile(self.log_path) else ["disabled"])

    def choose_game_files(self):
        if self.running:
            return
        self.st.verdict = None  # Even choosing the same repaired folder must verify it again.
        self.st.build_ok = False
        self.st.artifacts, self.st.records = [], []
        self.st.inputs += 1
        self.app.show(self.app.PAGES.index(FindPage))

    def start(self):
        if getattr(self.app, '_quitting', False):
            return      # Quit is stopping the build: Try again must not start a worker it would not wait for (TK-ST2)
        self.failed = None
        self.build = None
        self.stop.clear()
        self.log_path = None
        self.running = True
        self.st.invalidate_build()
        self.st.inputs += 1
        self.input_key = self.st.build_input_key()
        self.log.delete("1.0", "end")
        self.intro.config(text="Building...")
        self.status.config(text="")
        self.eta.config(text="")
        self.bar.config(value=0)
        self.buttons()
        self.app.update_nav()
        self.worker_done = threading.Event()     # this worker's own; it sets it when it ends (TK-D9)
        threading.Thread(target=self.worker, daemon=True).start()

    def worker(self):
        done = getattr(self, 'worker_done', None)   # taken now: a later start() makes the next worker its own
        try:
            for i, target in enumerate(self.st.targets):
                self.target_index = i
                self.app.call(self.intro.config, {"text": "Building for %s..." % SHORT_TARGET.get(target, target)})
                b = pipeline.Build(self.st.product, self.st.tc, self.app.opts.work, target, self.st.folder,
                                   store=self.st.verdict.result.store,
                                   outputs=self.st.outputs, openh264=self.st.openh264, parts=self.st.parts,
                                   on_line=lambda s: self.app.call(self.line, s))
                # App.poll samples this build's current progress once per tick. Queuing a redraw for every
                # compiler line lets a busy window accumulate thousands of redundant Tk callbacks.
                self.build = b
                self.log_path = b.log_path
                # Checked after self.build is set: a Cancel either sees this build or is seen here.
                if self.stop.is_set():
                    raise pipeline.BuildFailed(CANCELLED, self.log_path)
                arts, record = b.run()
                self.st.artifacts += arts
                self.st.records.append(record)
            self.app.call(self.finished, None)
        except pipeline.BuildFailed as e:
            self.app.call(self.finished, e)
        except Exception:  # noqa: BLE001 (short player message, full diagnostics in the support log)
            diagnostic = pipeline.filter_log(traceback.format_exc())
            paths = [self.log_path, os.path.join(self.app.opts.work, 'logs', 'builder-error.log')]
            self.log_path = None
            for path in paths:
                if not path:
                    continue
                try:
                    os.makedirs(os.path.dirname(path), exist_ok=True)
                    with open(path, 'a', encoding='utf-8') as log:
                        log.write('\n' + diagnostic)
                    self.log_path = path
                    break
                except OSError:
                    continue  # An unwritable work folder must still leave diagnostics in the window's log.
            for line in diagnostic.splitlines():
                self.app.call(self.line, line)
            self.app.call(self.finished, pipeline.BuildFailed("The build stopped unexpectedly. Choose Try again.",
                                                              self.log_path))
        finally:
            if done is not None:
                done.set()

    def line(self, s):
        self.log.insert("end", s + "\n")
        n = int(self.log.index("end-1c").split(".")[0])
        if n > 400:
            self.log.delete("1.0", "%d.0" % (n - 400))
        self.log.see("end")

    def progress(self, p):
        share = 100.0 / len(self.st.targets)
        within = p.completion()
        overall = share * self.target_index + share * within / 100
        self.bar.config(value=overall)
        self.eta.config(text="%s - %d%%" % (p.activity, int(round(overall))))
        # makerom exposes no intra-file percentage. Keep the overall bar honest
        # and animate activity while it packages rather than inventing an ETA.
        if p.activity == 'Making .cia' and not self.packaging.winfo_manager():
            self.packaging.pack(fill='x', pady=(0, 6), after=self.eta)
            self.packaging.start(16)
        elif p.activity != 'Making .cia':
            self.packaging.stop()
            self.packaging.pack_forget()

    def finished(self, err):
        self.running = False
        self.packaging.stop()
        self.packaging.pack_forget()
        if err is None and self.input_key is not None and self.input_key != self.st.build_input_key():
            self.st.invalidate_build()
            err = pipeline.BuildFailed('The game files or build tools changed. Choose Try again.', self.log_path)
        if err is None:
            self.st.build_ok = True
            self.st.build_key = self.input_key or self.st.build_input_key()
            self.st.outputs = pipeline.available_outputs(self.st.artifacts)
            self.intro.config(text="Build complete.")
            self.bar.config(value=100)
            self.eta.config(text="100%")
            lines = ["Built: %s." % ", ".join("%s %s" % (SHORT_TARGET.get(r["target"], r["target"]),
                                                          _fmt_secs(r["seconds"]))
                                               for r in self.st.records)]
            for a in self.st.artifacts:
                lines.append("%s (%.1f MB)" % (a.path, a.size / 1e6))
            for notice in dict.fromkeys(n for r in self.st.records for n in r.get('notices', [])):
                lines.append(notice)
            self.status.config(text="\n".join(lines))
        else:
            self.failed = err
            self.details.show()
            self.intro.config(text="Build stopped.")
            self.log_path = err.log_path if err.log_path and os.path.isfile(err.log_path) else None
            if getattr(err, 'details', ''):
                self.line(err.details)
            self.status.config(text=str(err) + ('\nLog: ' + self.log_path if self.log_path else ''))
        self.buttons()
        self.app.update_nav()

    def cancel(self):
        def stop():
            if self.running:        # The build may have finished while the question was open.
                self.stop.set()
                build = self.build
                if build is not None:
                    build.cancel()
        if self.running:
            self.app.ask("Cancel build", "Stop the build?", stop)

    def stop_for_quit(self, worker_seconds=None):
        """Quit (TK-D5, D9): stop the whole run, not one Build, and confirm it. Blocks, so never on the Tk thread.
        Returns why the build is not known to have stopped, or None. The stop is set before the current Build is
        read, as in Cancel (AUD-14), so the worker either starts no further Build or this sees the one it made;
        that Build's stop_and_confirm() then either keeps it from starting a process or waits for its release.
        No process of the build running is not the worker being done: it can still be copying sources or
        collecting outputs in the work folder. None is returned only once the worker has ended (TK-ST2)."""
        self.stop.set()
        build = self.build
        failure = build.stop_and_confirm() if build is not None else None
        if not self.worker_done.wait(WORKER_EXIT_SECONDS if worker_seconds is None else worker_seconds):
            return failure or "it was still finishing a step"
        return failure

    def copy_log(self):
        if not self.log_path:
            return
        try:
            with open(self.log_path, encoding="utf-8", errors="replace") as f:
                text = pipeline.filter_log(f.read())
        except OSError:
            self.log_path = None
            self.buttons()
            return
        self.app.root.clipboard_clear()
        self.app.root.clipboard_append(text)
        self.status.config(text=self.status.cget("text") + "\nLog copied.")

    def can_next(self):
        return self.st.build_ok and not self.running

    def busy(self):
        return self.running

    def auto(self):
        if self.running:
            return "wait"
        if self.failed:
            return ("fail", str(self.failed))
        return "next" if self.st.build_ok else "wait"


STALE_COPY = "The card list changed during the copy, so check the card and click Copy to card again."


def source_manifest(folder, product=None, parts=None):
    """Each file in the game folder with its size and modified and change (POSIX) or creation (Windows) times, so a
    game file replaced at the same path after a copy makes that copy stale (R175 F4). A missing folder lists nothing. A folder or link that can't be
    read is listed as such, so it never blocks Next on its own but changes the listing once it can be read."""
    if not folder or not os.path.isdir(folder):
        return ()
    if product is not None:
        try:
            return tuple((rel, info.st_size, info.st_mtime_ns, info.st_ctime_ns)
                         for source, rel, _ in products.sd_files(product, folder, parts)
                         for info in [os.stat(source)])
        except OSError as error:
            return (('game files', 'unreadable', error.errno),)
    out, todo = [], [(folder, '')]
    while todo:  # scandir: Windows gives size and date with the listing, so this stays quick on the Tk thread.
        top, rel = todo.pop()
        try:
            with os.scandir(top) as entries:
                for entry in entries:
                    if entry.is_dir(follow_symlinks=False):  # Never into a linked folder: no loops.
                        todo.append((entry.path, rel + entry.name + '/'))
                        continue
                    try:
                        info = entry.stat()
                    except OSError:
                        info = entry.stat(follow_symlinks=False)  # A broken link: its own size and date.
                    out.append((rel + entry.name, info.st_size, info.st_mtime_ns, info.st_ctime_ns))
        except OSError as error:
            out.append((rel, 'unreadable', error.errno))
    return tuple(sorted(out, key=repr))


class DeliverPage(Page):
    """Each way of delivering has its own panel and its own status text; a progress bar shows only while copying."""
    title = "Put it on the 3DS"
    card_roots = {}  # Listed card text -> its root; refresh() replaces it.
    # Bumped by forget() for each mode. A copy, send or save captures its mode's count when it starts, and only a
    # completion still current may grant a receipt: one that finishes after a Refresh, a folder choice or a new
    # visit reports where it went but doesn't enable Next (R175b F4).
    sd_visit = wifi_visit = files_visit = 0
    scanning = False
    scan_visit = 0
    checking_files = False
    file_visit = 0
    manifest_context = None
    file_manifest = None
    next_checked = None

    def __init__(self, app):
        super().__init__(app)
        self.how = tk.StringVar(value="sd")
        radiobutton(self.frame, text="Copy to SD card", value="sd", variable=self.how,
                    command=self.mode).pack(anchor="w")
        radiobutton(self.frame, text="Send files over Wi-Fi", value="wifi",
                    variable=self.how, command=self.mode).pack(anchor="w")
        radiobutton(self.frame, text="Save to a folder on this computer", value="files",
                    variable=self.how, command=self.mode).pack(anchor="w", pady=(0, 8))
        self.sd = ttk.Frame(self.frame)
        row = ttk.Frame(self.sd)
        row.pack(fill="x")
        row.columnconfigure(0, weight=1)
        self.card = ttk.Combobox(row, width=24, state="readonly")
        self.card.grid(row=0, column=0, sticky="ew", padx=(0, 8))
        button(row, text="Refresh", command=self.refresh).grid(row=0, column=1, padx=(0, 8))
        button(row, text="Choose folder...", command=self.choose).grid(row=0, column=2)
        self.go = button(self.sd, text="Copy to card", command=self.copy, primary=True)
        self.go.pack(anchor="w", pady=6)
        self.wifi = ttk.Frame(self.frame)
        label(self.wifi, '3DS address (shown by the ftpd app):')
        self.addr = ttk.Combobox(self.wifi, width=30)
        self.addr.pack(anchor="w", pady=(0, 6))
        self.addr.set(preferences.load(app.opts.work).get('console', ''))
        label(self.wifi, "Open ftpd, the file-transfer app, on your 3DS and keep it open while sending.")
        button(self.wifi, text='Get ftpd', command=lambda: webbrowser.open(
            'https://github.com/mtheall/ftpd#latest-builds')).pack(anchor='w', pady=(0, 6))
        row = ttk.Frame(self.wifi)
        row.pack(anchor="w", pady=6)
        self.find_button = button(row, text="Find my 3DS", command=self.find_console)
        self.find_button.pack(side='left', padx=(0, 8))
        self.send_button = button(row, text="Send", command=self.send, primary=True)
        self.send_button.pack(side='left', padx=(0, 8))
        self.cancel_button = button(row, text="Cancel", command=lambda: self.cancelled.set())
        self.cancel_button.pack(side='left')
        self.cancel_button.state(['disabled'])
        self.cancelled = threading.Event()
        self.files = ttk.Frame(self.frame)
        self.save = button(self.files, text="Save files", command=self.save_files, primary=True)
        self.save.pack(anchor="w", pady=(0, 8))
        self.open_files = button(self.files, text="Open output folder", command=lambda: open_folder(
            self.st.files_root))
        self.open_files.pack(anchor="w", pady=(0, 8))
        self.open_files.state(["disabled"])
        self.steps = label(self.files, "")
        self.panels = {"sd": self.sd, "wifi": self.wifi, "files": self.files}
        self.bar = progressbar(self.frame, mode="determinate", length=560, maximum=100)
        self.status = label(self.frame, "")
        self.status.pack_forget()
        self.texts = {"sd": "", "wifi": "", "files": ""}
        self.bar_used = {"sd": False, "wifi": False, "files": False}
        self.copying = False
        self.cards = []
        self.manual = None
        self.saved_key = None
        self.saved_textures = None
        self.scanning = False
        self.scan_visit = 0
        self.file_status = label(self.frame, '')
        self.checking_files = False
        self.file_visit = 0
        self.manifest_context = self.file_manifest = self.next_checked = None

    def on_show(self):
        # A copy or send counts only on the visit it was made in: the card or console may be another one now (R175 F4).
        self.forget()
        self.refresh()
        self.mode()

    def forget(self, how=None):
        """Drop the finished copy or send (of one mode, or any), and any still running: it must be done again
        before Next."""
        self.file_visit += 1
        self.checking_files = False
        self.next_checked = None
        status = getattr(self, 'file_status', None)
        if status is not None:
            status.config(text='')
        for mode in (how,) if how else ('sd', 'wifi', 'files'):
            setattr(self, mode + '_visit', getattr(self, mode + '_visit') + 1)
        if how in (None, 'files'):
            self.saved_key = None  # mode() rebuilds the files receipt from this, so it goes too (R175c).
            self.saved_textures = None
        if how is None or (self.st.delivered_key or (None,))[0] == how:
            self.st.delivered, self.st.delivered_key = False, None
        if how is None or self.st.deliver_how == how:
            self.st.new_delivery()

    def set_status(self, how, text):
        """One status text per panel; only the visible panel's text is on screen."""
        self.texts[how] = text
        if how == self.how.get():
            self._show_status()

    def log_delivery(self, text):
        try:
            path = self.app.pages[5].log_path or os.path.join(self.app.opts.work, 'logs', 'delivery.log')
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'a', encoding='utf-8', errors='replace', newline='\n') as log:
                log.write(text.rstrip() + '\n')
        except OSError:
            pass  # A full or unavailable log destination must never interrupt delivery.

    def remember_textures(self, result):
        self.st.texture_result = result
        if result is not None:
            self.log_delivery(result.message)

    def _show_status(self):
        how = self.how.get()
        panel = self.panels[how]
        if not panel.winfo_manager():
            for widget in (self.bar, self.status):
                if widget.winfo_manager():
                    widget.pack_forget()
            self._status_layout = None
            return
        text = self.texts[how]
        if self.status.cget("text") != text:
            self.status.config(text=text)
        layout = (how, self.bar_used[how], bool(text))
        if (layout == getattr(self, '_status_layout', None)
                and bool(self.bar.winfo_manager()) == bool(self.bar_used[how])
                and bool(self.status.winfo_manager()) == bool(text)):
            return  # Progress text must not unmap/remap the bar or the heart.
        anchor = panel
        if self.bar_used[how]:
            self.bar.pack(fill="x", pady=(4, 6), after=anchor)
            anchor = self.bar
        elif self.bar.winfo_manager():
            self.bar.pack_forget()
        if text:
            self.status.pack(anchor="w", pady=(0, 6), after=anchor)
        elif self.status.winfo_manager():
            self.status.pack_forget()
        self._status_layout = layout

    def show_progress(self, how, value):
        self.bar_used[how] = True
        self.bar.config(value=value)
        if how == self.how.get() and not self.bar.winfo_ismapped():
            self._show_status()

    def mode(self):
        self.check_files()

    def show_mode(self):
        how = self.how.get()
        for name, panel in self.panels.items():
            if name != how and panel.winfo_manager():
                panel.pack_forget()
        key = self.files_key()
        if how != self.st.deliver_how or (key is not None and self.st.delivered_key != self.delivery_key()):
            self.st.delivered = False  # Another mode, build, game, chapter set or destination: copy again.
            self.st.new_delivery()
        self.st.deliver_how = how
        if not self.panels[how].winfo_manager():
            self.panels[how].pack(anchor="w", fill="x")
        if how == "files":
            ready = key is not None and self.saved_key == key and bool(self.st.files_root)
            self.steps.config(text=("Output: %s\nRead READ-ME-FIRST.txt before copying. Use Copy to SD card for a safe merge." % self.st.files_root
                                    if ready else "Save the build, game files and prepared textures in one folder."))
            self.open_files.state(["!disabled"] if ready else ["disabled"])
            self.st.sd_root = None
            self.st.delivered = ready
            self.st.delivered_key = self.delivery_key() if ready else None
            self.st.texture_result = getattr(self, 'saved_textures', None) if ready else None
        self._show_status()
        self.app.update_nav()

    def file_context(self):
        """Only in-memory build choices; disk checks belong to check_files' worker."""
        return (self.st.game, self.st.folder, tuple(self.st.parts or ()),
                tuple((a.path, a.sha256) for a in self.st.artifacts), self.st.inputs)

    def files_key(self):
        context = self.file_context()
        return context + (self.file_manifest,) if context == self.manifest_context else None

    def check_files(self, done=None):
        """A sleeping game disk must not block Tk. A new visit, mode or input supersedes this result."""
        self.file_visit += 1
        visit, context, how = self.file_visit, self.file_context(), self.how.get()
        index = getattr(self.app, 'index', None)
        destination = self.destination()
        folder, product, parts = self.st.folder, self.st.product, tuple(self.st.parts) if self.st.parts else None
        self.checking_files = True
        status = getattr(self, 'file_status', None)
        if status is not None:
            status.config(text='Checking game files...')
        self.show_mode()

        def after(manifest):
            if visit != self.file_visit:
                return
            self.checking_files = False
            if (context != self.file_context() or how != self.how.get() or
                    index != getattr(self.app, 'index', None)):
                return
            if isinstance(manifest, Exception):
                self.manifest_context = self.file_manifest = None
                if status is not None:
                    status.config(text='Could not check game files. Choose the delivery method again to retry.')
            else:
                self.manifest_context, self.file_manifest = context, manifest
                if status is not None:
                    status.config(text='')
            self.show_mode()
            if done is not None and not isinstance(manifest, Exception) and destination == self.destination():
                done()
        self.app.work(lambda: source_manifest(folder, product, parts), after, errors=True)

    def destination(self):
        how = self.how.get()
        if how == 'sd':
            root = self.target_root()
            return os.path.normcase(os.path.normpath(root)) if root else None
        if how == 'wifi':
            try:
                return '%s:%d' % ftp_delivery.endpoint(self.addr.get())
            except OSError:
                return None
        return self.st.files_root

    def delivery_key(self):
        """What a finished copy was: the mode, the card, console or folder, and the build and game files."""
        return self.how.get(), self.destination(), self.files_key()

    def save_files(self):
        if self.copying:
            return
        if self.files_key() is None:
            self.check_files(self.save_files)
            return
        output_parent = None
        if self.app.opts.autopilot is None:
            output_parent = filedialog.askdirectory(title='Choose where to save the card files',
                initialdir=os.path.dirname(self.st.files_root) if self.st.files_root else os.path.expanduser('~/Desktop'),
                parent=self.app.root)
            if not output_parent:
                return
        self.st.new_delivery()
        key, visit = self.files_key(), self.files_visit
        self.copying = True
        self.st.delivered = False
        self.saved_key = None  # Until this save succeeds: a failed one may have changed the folder (R175c).
        self.saved_textures = None
        self.save.state(["disabled"])
        self.open_files.state(["disabled"])
        self.set_status("files", "Saving files and preparing textures...")
        self.show_progress("files", 0)
        self.app.update_nav()
        prog = lambda d, t, n: self.app.call(self.show_progress, "files", 100.0 * d / max(1, t))

        def after(res):
            self.copying = False
            self.save.state(["!disabled"])
            if isinstance(res, Exception):
                self.set_status("files", str(res))
            else:
                if visit == self.files_visit:
                    self.saved_key = key
                    self.saved_textures = res.textures
                self.st.files_root = getattr(res, 'folder', None) or res.root
                self.show_progress("files", 100)
                self.set_status("files", "Saved %d files (%.1f MB).\n%s" %
                                (res.files, res.bytes / 1e6, res.textures.message))
            self.mode()
            if not isinstance(res, Exception) and visit == self.files_visit:
                self.remember_textures(res.textures)
        self.app.work(lambda: deliver.save_files(self.st.product, self.st.artifacts, self.st.folder,
                                                 self.app.opts.work, self.st.parts, prog,
                                                 output_parent=output_parent, log=self.log_delivery), after, errors=True)

    def refresh(self):
        self.forget('sd')  # A card may have been swapped at the same drive or mount point (R175 F4).
        self.scan_visit += 1
        visit = self.scan_visit
        self.scanning = True
        self.cards, self.card_roots = [], {}
        self.card.config(values=[])
        self.card.set('')
        self.go.state(['disabled'])
        self.set_status('sd', 'Looking for a 3DS SD card...')
        self.app.update_nav()

        def after(cards):
            if visit != self.scan_visit:
                return  # A newer Refresh or folder choice owns the list now.
            self.scanning = False
            self.cards = [] if isinstance(cards, Exception) else cards
            self.show_cards()
            self.app.update_nav()
        self.app.work(deliver.list_sd_cards, after, errors=True)

    def show_cards(self):
        vals = ["%s  %s (%d MB free)" % (c.root, c.label, c.free // 10**6) for c in self.cards]
        # The root is looked up, never parsed back out of the text: a card's name may hold two spaces (SCOURS1-003).
        self.card_roots = dict(zip(vals, (c.root for c in self.cards)))
        if self.manual:
            vals.append(self.manual)
        self.card.config(values=vals)
        if vals:
            self.card.current(len(vals) - 1 if self.manual else 0)
            self.set_status("sd", "")
        else:
            self.card.set("")
            self.set_status("sd", "No 3DS SD card found.\nInsert your SD card and click Refresh, or Choose folder...")
        self.copy_button()

    def copy_button(self):
        self.go.state(['!disabled'] if not self.copying and not self.scanning and self.target_root()
                      else ['disabled'])

    def choose(self):
        d = filedialog.askdirectory(title="Select 3DS SD card")
        if d:
            self.set_manual(d)

    def set_manual(self, d):
        self.manual = os.path.normpath(d)
        self.refresh()

    def target_root(self):
        v = self.card.get()
        if not v:
            return None
        if self.manual and v == self.manual:
            return self.manual
        return self.card_roots.get(v) or v.split("  ")[0]

    def copy(self):
        if self.copying or self.scanning:
            return
        if self.files_key() is None:
            self.check_files(self.copy)
            return
        root = self.target_root()
        if not root:
            return
        self.st.new_delivery()
        visit = self.sd_visit
        self.copying = True
        self.st.delivered = False  # Until this copy succeeds, even if an earlier one did.
        self.go.state(["disabled"])
        self.set_status('sd', 'Preparing a list of files. Nothing is being copied to the card yet.')
        self.app.update_nav()
        def prepared(result):
            if isinstance(result, Exception):
                self.copying = False
                self.copy_button()
                self.set_status('sd', str(result))
                self.app.update_nav()
                return
            items, tool, text = result
            # Autopilot only writes to its explicit disposable card fixture.
            if self.app.opts.autopilot is not None:
                self._copy_approved(root, items, tool, visit)
                return
            def dismiss():
                self.copying = False
                self.copy_button()
                self.set_status('sd', 'Copy cancelled. Nothing was copied.')
                self.app.update_nav()
            changed = [item for item in items if not getattr(item, 'skip', False)]
            summary = ('Game and build files to copy: %d (%.1f MB).\n'
                       'Only missing or changed files will be copied. '
                       'Saves, settings, other homebrew and other games are kept.' %
                       (len(changed), sum(getattr(item, 'size', 0) for item in changed) / 1e6))
            if getattr(text, 'texture_fallback', False):
                summary += '\n\n' + deliver.TEXTURE_FALLBACK
            self.app.prompt('Files for this SD card', summary,
                            [('Cancel', dismiss, False),
                             ('Copy to card', lambda: self._copy_approved(root, items, tool, visit), True)],
                            details=str(root) + '\n\n' + text, cancel='Cancel', focus='Copy to card',
                            collapse_details=True)
        self.app.work(lambda: deliver.sd_preview(root, self.st.product, self.st.artifacts,
                      self.st.folder, self.app.opts.work, self.st.parts, log=self.log_delivery), prepared, errors=True)

    def _copy_approved(self, root, items, tool, visit=None):
        if self.files_key() is None:
            self.check_files(lambda: self._copy_approved(root, items, tool, visit))
            return
        self.st.new_delivery()
        self.set_status("sd", "Copying...")
        self.show_progress("sd", 0)
        self.app.update_nav()
        prog = lambda done, total, name: self.app.call(self.show_progress, "sd", 100.0 * done / max(1, total))  # noqa
        key = self.files_key()
        visit = self.sd_visit if visit is None else visit

        def after(res):
            self.copying = False
            self.copy_button()
            if isinstance(res, Exception):
                self.set_status("sd", str(res))
            else:
                copied, skipped, nbytes, tex = res
                self.show_progress("sd", 100)
                msg = "Copied %d files (%.1f MB) to %s (%d unchanged)." % (copied, nbytes / 1e6, root, skipped)
                msg += "\n" + tex.message
                if deliver.dsp_missing(root):
                    msg += "\n\n" + deliver.DSP_MESSAGE
                if visit != self.sd_visit:
                    msg += "\n\n" + STALE_COPY
                self.set_status("sd", msg)
                if visit == self.sd_visit:
                    self.remember_textures(tex)
                    self.st.sd_root = root
                    self.st.deliver_how = "sd"
                    self.st.delivered = True
                    self.st.delivered_key = ('sd', os.path.normcase(os.path.normpath(root)), key)
            self.check_files()
        self.app.work(lambda: deliver.copy_with_textures(root, items, self.st.product, self.st.folder,
                                                         self.app.opts.work, self.st.parts, prog, tool=tool,
                                                         log=self.log_delivery), after, errors=True)

    def send(self):
        if self.copying:
            return
        if self.files_key() is None:
            self.check_files(self.send)
            return
        try:
            host, port = ftp_delivery.endpoint(self.addr.get())
            addr = '%s:%d' % (host, port)
        except OSError as exc:
            self.set_status('wifi', str(exc))
            return
        self.st.new_delivery()
        self.wifi_busy(True)
        self.st.delivered = False
        self._ftp_update, self._ftp_phase = 0, None
        key, visit = self.files_key(), self.wifi_visit
        self.set_status('wifi', 'Preparing files and textures on the PC...')
        self.show_progress("wifi", 0)

        def job():
            return ftp_delivery.send_product(addr, self.st.product, self.st.artifacts, self.st.folder,
                    self.app.opts.work, self.st.parts, cancelled=self.cancelled.is_set,
                    log=self.log_delivery, progress=self.ftp_progress)

        def after(res):
            self.wifi_busy(False)
            self.set_status("wifi", ftp_delivery.error_text(res) if isinstance(res, Exception) else "Sent and checked. You can close ftpd.")
            if not isinstance(res, Exception):
                self.show_progress("wifi", 100)
                if getattr(res, 'textures', None) is not None:
                    self.set_status('wifi', 'Sent and checked. You can close ftpd.\n' + res.textures.message)
            if not isinstance(res, Exception) and visit == self.wifi_visit:
                self.remember_textures(getattr(res, 'textures', None))
                self.st.sd_root = None
                self.st.ftp_console = addr
                self.st.files_root = res.root
                self.st.deliver_how = "wifi"
                self.st.delivered = True
                self.st.delivered_key = ('wifi', addr, key)
                preferences.save(self.app.opts.work, self.app.boring.get(), console=addr)
            self.check_files()
        self.app.work(job, after, errors=True)

    def wifi_busy(self, busy):
        self.copying = busy
        if busy:
            self.cancelled.clear()
        for widget in (self.find_button, self.send_button, self.addr):
            widget.state(['disabled'] if busy else ['!disabled'])
        self.cancel_button.state(['!disabled'] if busy else ['disabled'])
        self.app.update_nav()

    def ftp_progress(self, event):
        if event.phase == 'Finished':
            return  # Completion callback marks 100 only after send returns successfully.
        now = __import__('time').monotonic()
        phase = 'Checking files' if event.phase.startswith('Checking') else 'Sending'
        if phase == getattr(self, '_ftp_phase', None) and now - getattr(self, '_ftp_update', 0) < 0.15:
            return
        self._ftp_update, self._ftp_phase = now, phase
        self.app.call(self._show_ftp_progress, min(99, int(event.percent)), phase)

    def _show_ftp_progress(self, percent, phase):
        self.show_progress('wifi', percent)
        self.set_status('wifi', '%d%% - %s' % (percent, phase))

    def find_console(self, permission_retry=False):
        if self.copying:
            return
        self.forget('wifi')  # Looking again: the console at that address may be another one (R175 F4).
        self.wifi_busy(True)
        self.set_status('wifi', 'Looking for a 3DS running ftpd on your network...' +
            ('\nIf macOS asks for Local Network access, choose Allow.' if sys.platform == 'darwin' else ''))
        def job():
            networks = ftp_delivery.local_networks()
            return ftp_delivery.find_consoles(networks, allowed=networks, cancelled=self.cancelled.is_set,
                progress=lambda d, t: self.app.call(self.set_status, 'wifi', 'Checked %d of %d addresses.' % (d, t)))
        def after(found):
            self.wifi_busy(False)
            if isinstance(found, Exception):
                self.set_status('wifi', ftp_delivery.error_text(found, connecting=True))
            elif not found:
                self.set_status('wifi', 'No 3DS found. Check that ftpd is open and the 3DS is on the same network as this PC, or type the address ftpd shows.' +
                    ('\nLocal Network access may be off. Open System Settings > Privacy & Security > Local Network and allow this builder, then try Find my 3DS again.' if sys.platform == 'darwin' else ''))
                # TCP cannot distinguish a privacy refusal from an unreachable
                # console. Explain that uncertainty, then offer one explicit
                # retry after the player has allowed access, never a scan loop.
                if sys.platform == 'darwin' and not permission_retry:
                    self.app.ask('Find my 3DS',
                            'No 3DS answered. If Local Network access was blocked, allow this builder in System Settings > Privacy & Security > Local Network, then choose Retry.',
                            lambda: self.find_console(permission_retry=True), yes_text='Retry', no_text='Cancel',
                            safe=False)
            else:
                self.addr.config(values=found)
                self.addr.set(found[0])
                self.set_status('wifi', 'Found a 3DS. Check that the address matches the one ftpd shows, then click Send.')
        # Paint and finish the button-release event before discovery can trigger
        # macOS's Local Network prompt. Never update Tk from a probe thread.
        self.app.root.after_idle(lambda: self.app.work(job, after, errors=True))

    def can_next(self):
        return (not self.checking_files and self.files_key() is not None and self.st.delivered
                and self.st.delivered_key == self.delivery_key() and not self.copying)

    def on_next(self):
        key = self.delivery_key()
        if self.next_checked == (self.file_visit, key):
            self.next_checked = None
            return True

        def checked():
            if self.can_next() and self.delivery_key() == key:
                self.next_checked = (self.file_visit, key)
                self.app.next()
        self.check_files(checked)
        return False

    def busy(self):
        return self.copying

    def auto(self):
        ap = self.app.opts.autopilot or {}
        if self.copying or self.scanning or self.checking_files:
            return "wait"
        if self.can_next():
            return "next"
        if ap.get("deliver", "sd") == "files":
            self.how.set("files")
            self.mode()
            if not getattr(self, "_files_auto_started", False):
                self._files_auto_started = True
                self.save_files()
                return "wait"
            return ("fail", self.texts["files"])
        if ap.get("sd"):
            if self.target_root() != os.path.normpath(ap["sd"]):
                self.set_manual(ap["sd"])
                return "wait"
            if not getattr(self, "_auto_started", False):
                self._auto_started = True
                self.copy()
                return "wait"
            return ("fail", self.texts["sd"])
        return ("fail", "no SD path in the autopilot plan")


KEPT_SAVES = "3DS saves kept.\nSelect Skip to go on."


class SavesPage(Page):
    title = "Import PC saves"

    def __init__(self, app):
        super().__init__(app)
        self.replace_agreed = None      # the replace-saves question the player said Yes to (confirm_replace)
        self.intro = label(self.frame, "")
        self.folder_label = label(self.frame, "", bold=True)
        row = ttk.Frame(self.frame)
        row.pack(anchor="w", pady=(0, 8))
        self.scan_button = button(row, text="Scan again", command=self.scan)
        self.scan_button.pack(side="left", padx=(0, 8))
        self.choose_button = button(row, text="Choose folder...", command=self.choose)
        self.choose_button.pack(side="left")
        self.tree = ttk.Treeview(self.frame, columns=("file", "date", "summary", "status"), show="headings", height=6,
                                 selectmode="none")
        for c, w, heading in (("file", 90, "File"), ("date", 150, "Saved"),
                              ("summary", 180, "Name / LV"), ("status", 110, "Status")):
            self.tree.heading(c, text=heading)
            self.tree.column(c, width=w, minwidth=60, stretch=(c == "summary"))
        self.tree.pack(fill="x", pady=(0, 8))
        self.notice = label(self.frame, "")
        self.choice = tk.StringVar(master=self.frame, value="skip")
        self.copy_radio = radiobutton(self.frame, text="Copy PC saves to SD card", value="copy",
                                      variable=self.choice, command=self.mode)
        self.copy_radio.pack(anchor="w")
        self.wifi_radio = radiobutton(self.frame, text="Send PC saves over Wi-Fi", value="wifi",
                                      variable=self.choice, command=self.mode)
        self.wifi_radio.pack(anchor="w")
        self.skip_radio = radiobutton(
            self.frame, text="Skip", value="skip",
            variable=self.choice, command=self.mode)
        self.skip_radio.pack(anchor="w", pady=(0, 8))
        self.sd = ttk.Frame(self.frame)
        self.card = ttk.Combobox(self.sd, width=40, state='readonly')
        self.card.pack(side='left', padx=(0, 8))
        self.card.bind('<<ComboboxSelected>>', lambda _e: self.scan())
        self.refresh_button = button(self.sd, text='Refresh', command=self.refresh_cards)
        self.refresh_button.pack(side='left', padx=(0, 8))
        self.card_button = button(self.sd, text='Choose folder...', command=self.choose_card)
        self.card_button.pack(side='left')
        self.wifi = ttk.Frame(self.frame)
        label(self.wifi, '3DS address (shown by the ftpd app):')
        self.addr = ttk.Combobox(self.wifi, width=30)
        self.addr.pack(anchor='w', pady=(0, 6))
        label(self.wifi, 'Open ftpd, the file-transfer app, on your 3DS and keep it open while sending.')
        button(self.wifi, text='Get ftpd', command=lambda: webbrowser.open(
            'https://github.com/mtheall/ftpd#latest-builds')).pack(anchor='w', pady=(0, 6))
        self.check_button = button(self.wifi, text='Check saves', command=self.check_ftp)
        self.check_button.pack(anchor='w')
        self.addr.bind('<<ComboboxSelected>>', lambda _e: self.check_ftp())
        self.addr.bind('<Return>', lambda _e: self.check_ftp())
        self.addr.bind('<KeyRelease>', lambda _e: self.address_changed())
        self.bar = progressbar(self.frame, mode='determinate', length=560, maximum=100)
        self.status = label(self.frame, "")
        self.bar.pack_forget()
        self.card_roots = {}
        self.checked_console = None
        self.completed_key = None
        self.last_method = 'copy'
        self.custom_dir = None
        self.ftp_snapshot = None
        self.ftp_running = False
        self.ftp_resume = None  # A stopped import's state: the next import finishes it with the same backup.
        # Each FTP check or import is one job with its own number and Cancel flag. Only the newest job's completion
        # counts, and nothing starts another job while an import is replacing 3DS saves.
        self.ftp_job = 0
        self.ftp_importing = False
        self.ftp_cancel = threading.Event()
        self.ftp_cancel_button = button(self.frame, text='Cancel', command=lambda: self.ftp_cancel.set())
        self.ftp_cancel_button.pack_forget()

    def on_show(self):
        self.intro.config(text="Copy your PC saves to the 3DS. Your PC files are not changed.")
        self.card.set(self.st.sd_root or '')
        self.addr.set(self.st.ftp_console or preferences.load(self.app.opts.work).get('console', ''))
        self.choice.set('wifi' if self.st.deliver_how == 'wifi' and self.st.ftp_console else
                        'copy' if self.st.deliver_how == 'sd' and self.st.sd_root else 'skip')
        if self.st.current_save_result():
            self.choice.set('skip')
        self.refresh_cards(scan=False)
        self.last_method = self.choice.get()
        self.scan()

    def refresh_cards(self, scan=True):
        if self.importing_notice():
            return
        selected = self.card_root()
        try:
            cards = deliver.list_sd_cards()
        except OSError:
            cards = []
        self.card_roots = {'%s  %s (%d MB free)' % (c.root, c.label, c.free // 10**6): c.root for c in cards}
        values = list(self.card_roots)
        display = next((text for text, root in self.card_roots.items() if root == selected), selected)
        if selected and display not in values:
            values.append(selected)
        self.card.config(values=values)
        self.card.set(display)
        if scan:
            self.scan()

    def choose_card(self):
        if self.importing_notice():
            return
        folder = filedialog.askdirectory(title='Select 3DS SD card')
        if folder:
            self.card.set(os.path.normpath(folder))
            self.refresh_cards()

    def card_root(self):
        value = self.card.get()
        return self.card_roots.get(value, value)

    def mode(self):
        if self.importing_notice():
            return
        self.completed_key = None
        self.st.saves_done = False
        self.bar.pack_forget()
        if self.choice.get() == 'skip':
            self.new_ftp_job()
            self.ftp_running = False
            self.ftp_cancel_button.pack_forget()
            self.sd.pack_forget()
            self.wifi.pack_forget()
            self.status.config(text=('PC saves were already copied. Click Next to go on.' if self.st.current_save_result()
                                     else 'PC saves will not be copied.'))
            self.changed()
            return
        self.scan(keep_choice=True)

    def address_changed(self):
        if self.addr.get() != self.checked_console and not self.ftp_importing:
            self.completed_key = None
            self.ftp_snapshot = None
            self.new_ftp_job()
            self.ftp_running = False
            self.status.config(text='Click Check saves to check this 3DS before copying.')
            self.changed()

    def target_save_dir(self):
        if self.custom_dir:
            return self.custom_dir
        return self.app.opts.probes.pc_save_dir(self.st.game)

    def new_ftp_job(self):
        """Supersedes the current FTP job: it is cancelled and its completion will be ignored."""
        self.ftp_cancel.set()
        self.ftp_cancel = threading.Event()
        self.ftp_job += 1
        return self.ftp_job, self.ftp_cancel

    def importing_notice(self):
        if self.ftp_importing:
            self.status.config(text='Wait for the copy to finish, or click Cancel.')
            return True
        return False

    def choose(self):
        if self.importing_notice():
            return
        d = filedialog.askdirectory(title="Select %s PC save folder" % self.st.product.title)
        if d and not self.importing_notice():
            self.custom_dir = os.path.normpath(d)
            self.scan()

    def scan(self, keep_choice=False):
        if self.importing_notice():
            return  # The import uses this page's scan and snapshot; never change them under it.
        self.new_ftp_job()
        self.ftp_running = False
        self.ftp_cancel_button.pack_forget()
        p = self.st.product
        self.completed_key = None
        self.st.saves_done = False
        self.bar.pack_forget()
        self.sd.pack_forget()
        self.wifi.pack_forget()
        selected = self.choice.get()
        method = selected if selected != 'skip' else self.last_method
        self.last_method = method
        if selected == 'copy':
            self.sd.pack(fill='x', before=self.status, pady=(0, 8))
        elif selected == 'wifi':
            self.wifi.pack(fill='x', before=self.status, pady=(0, 8))
        folder = self.target_save_dir()
        scan = deliver.scan_saves(folder, p.key)
        self.st.save_scan = scan
        self.folder_label.config(text="PC saves: %s" % scan.folder)
        self.tree.delete(*self.tree.get_children())
        for i, s in enumerate(scan.files):
            status = "OK" if s.valid else "Unreadable"
            self.tree.insert("", "end", iid=str(i), values=(s.name, s.mtime_text, s.summary, status))
        valid = scan.valid_files
        corrupt = scan.corrupt_files
        notes = []
        if scan.error:
            notes.append(scan.error)
        elif not scan.exists or (not valid and not corrupt):
            notes.append("No PC saves were found in %s." % scan.folder)
        elif corrupt:
            notes.append("Can't read %s.\n"
                         "Choose another folder or select Skip." % ", ".join(s.name for s in corrupt))
        else:
            notes.append("Found %d PC save file%s." % (len(valid), "" if len(valid) == 1 else "s"))
        root = self.card_root()
        try:
            existing = deliver.existing_sd_save_files(root, p) if root and method == 'copy' else []
        except deliver.DeliverError as exc:
            self.notice.config(text=str(exc))
            self.choice.set('skip')
            self.changed()
            return
        existing_names = [os.path.basename(x) for x in existing]
        if existing:
            notes.append("SD card already has 3DS saves (%s); copying backs them up first."
                         % ", ".join(existing_names))
        wifi = method == 'wifi'
        if not root and method == 'copy':
            notes.append('Select your SD card, or choose Send PC saves over Wi-Fi.')
        self.notice.config(text="\n".join(notes))
        for radio in (self.copy_radio, self.wifi_radio):
            radio.state(['!disabled'] if valid else ['disabled'])
        if not valid or corrupt or (existing and not keep_choice):
            self.choice.set("skip")
        self.status.config(text='PC saves were already copied. Click Next to go on.'
                           if self.st.current_save_result() and self.choice.get() == 'skip' else '')
        self.changed()
        if wifi and self.choice.get() != 'skip':
            self.check_ftp(default_skip=not keep_choice)

    def check_ftp(self, default_skip=False):
        if self.importing_notice():
            return
        self.completed_key = None
        self.ftp_snapshot = None
        job, cancel = self.new_ftp_job()
        try:
            host, port = ftp_delivery.endpoint(self.addr.get())
        except OSError as exc:
            self.ftp_running = False
            self.status.config(text=str(exc))
            self.changed()
            return
        console = '%s:%d' % (host, port)
        self.checked_console = self.addr.get()
        self.ftp_running = True
        self.status.config(text='Checking the 3DS saves...')
        self.ftp_cancel_button.pack(anchor='w')
        def after(result):
            if job != self.ftp_job:
                return
            self.ftp_running = False
            self.ftp_cancel_button.pack_forget()
            if isinstance(result, Exception):
                self.status.config(text=ftp_saves.error_text(result, connecting=True))
            else:
                self.ftp_snapshot = result
                if result.files and default_skip:
                    self.choice.set('skip')
                self.status.config(text=('3DS saves found. Copying will back them up first.' if result.files else
                                         'No 3DS saves found.'))
            self.changed()
        resume = self.ftp_resume
        self.app.work(lambda: ftp_saves.inspect(console, self.st.product, cancelled=cancel.is_set, resume=resume),
                      after, errors=True)
        self.changed()

    def changed(self):
        self.app.update_nav()

    def confirm_replace(self, prompt, answer):
        """Whether the player agreed to replace the 3DS saves that `answer` names. With no test hook, the window asks
        and returns False; its Yes runs Next again, which then finds this answer agreed (once)."""
        agreed, self.replace_agreed = getattr(self, "replace_agreed", None), None
        if agreed is not None and agreed == answer:
            return True
        hook = self.app.opts.confirm_overwrite
        if hook is not None:
            agreed = hook("Replace 3DS saves?", prompt)
        else:
            def yes():
                self.replace_agreed = answer
                self.app.next()
            self.app.ask("Replace 3DS saves?", prompt, yes, lambda: self.status.config(text=KEPT_SAVES))
            return False
        if not agreed:
            self.status.config(text=KEPT_SAVES)
        return bool(agreed)

    def on_next(self):
        if self.busy():
            return False
        if self.choice.get() == 'skip':
            self.st.saves_done = True
            return True
        if self.completed_key == self.copy_key():
            return True
        self.status.config(text="")
        if self.choice.get() == 'wifi':
            if self.checked_console != self.addr.get() or self.ftp_snapshot is None:
                self.check_ftp()
                return False
            return self.import_ftp()
        root = self.card_root()
        if not root:
            self.status.config(text='Select your 3DS SD card, or choose Send PC saves over Wi-Fi.')
            return False
        p = self.st.product
        try:
            existing = deliver.existing_sd_save_files(root, p)
        except deliver.DeliverError as exc:
            self.status.config(text=str(exc))
            return False
        if existing and not self.st.save_scan.has_corrupt:
            sd_who = (deliver.scan_sd_saves(root, p).player_summary
                      or ", ".join(os.path.basename(x) for x in existing))
            pc_who = self.st.save_scan.player_summary or "PC files"
            prompt = ("Replace the 3DS save (%s) on %s with the PC save (%s)?\n"
                      "The 3DS saves are backed up first." % (sd_who, root, pc_who))
            if not self.confirm_replace(prompt, ('sd', root, tuple(existing), prompt,
                                                 self.st.save_scan)):
                return False
        scan = self.st.save_scan
        job, cancel = self.start_copy()
        key = self.copy_key()
        self.app.work(lambda: deliver.copy_pc_saves(root, p, scan, overwrite=bool(existing),
            cancelled=cancel.is_set,
            progress=lambda phase, done, total, name: self.app.call(self.show_progress, job, phase, done, total)),
            lambda result: self.finish_copy(job, key, result), errors=True)
        return False

    def copy_key(self):
        how = self.choice.get()
        destination = (self.addr.get() if how == 'wifi' else
                       os.path.normcase(os.path.normpath(self.card_root())))
        return how, destination, self.st.save_scan, self.st.delivery_context()

    def start_copy(self):
        job, cancel = self.new_ftp_job()
        self.ftp_running = self.ftp_importing = True
        self.st.saves_done = False
        for control in self.copy_controls():
            control.state(['disabled'])
        self.ftp_cancel_button.pack(anchor='w')
        self.bar.config(value=0)
        self.bar.pack(fill='x', before=self.status, pady=(4, 6))
        self.status.config(text='Checking saves and preparing the backup...')
        self.app.root.after_idle(lambda: self.show_widget(self.status))
        self.app.update_nav()
        return job, cancel

    def copy_controls(self):
        return (self.scan_button, self.choose_button, self.copy_radio, self.wifi_radio, self.skip_radio,
                self.card, self.card_button, self.refresh_button, self.addr, self.check_button)

    def show_progress(self, job, phase, done, total):
        if job != self.ftp_job or not self.ftp_importing:
            return
        self.bar.config(value=min(99, 100 * done / max(1, total)))
        self.status.config(text='%s: %d of %d bytes checked or copied.' % (phase, done, total))

    def finish_copy(self, job, key, result):
        if job != self.ftp_job:
            return
        self.ftp_running = self.ftp_importing = False
        for control in self.copy_controls():
            control.state(['!disabled'])
        self.card.state(['readonly'])
        self.ftp_cancel_button.pack_forget()
        if isinstance(result, Exception):
            if isinstance(result, ftp_saves.IncompleteImport):
                self.ftp_resume = result
            self.status.config(text=ftp_saves.error_text(result) if key[0] == 'wifi' else str(result))
        else:
            self.ftp_resume = None
            if key[3] != self.st.delivery_context():
                self.status.config(text='The delivery changed. Check this destination and copy the saves again.')
                self.app.update_nav()
                return
            self.st.save_result = result
            self.st.save_how = 'wifi' if key[0] == 'wifi' else 'sd'
            self.st.save_destination = key[1]
            self.st.save_receipt = key[3], self.st.save_how, key[1]
            self.st.saves_done = True
            self.completed_key = key
            self.bar.config(value=100)
            text = 'Copied %d save%s %s.' % (result.copied, '' if result.copied == 1 else 's',
                'to the 3DS over Wi-Fi' if key[0] == 'wifi' else 'to the SD card')
            if result.backup_dir:
                text += '\nPrevious 3DS saves backed up to: %s' % result.backup_dir
            self.status.config(text=text + '\nClick Next to go on.')
        self.app.root.after_idle(lambda: self.show_widget(self.status))
        self.app.update_nav()

    def import_ftp(self):
        if self.ftp_running or self.ftp_snapshot is None:
            return False
        if self.st.save_scan.has_corrupt or not self.st.save_scan.valid_files:
            self.status.config(text='No readable PC saves to copy, so nothing was copied. Choose another folder or skip.')
            return False
        existing = self.ftp_snapshot
        if existing.files:
            prompt = ('Replace the 3DS save (%s) at %s with the PC save (%s)?\n'
                      'The 3DS saves are backed up first.' %
                      (existing.player_summary or ', '.join(existing.files), self.addr.get(),
                       self.st.save_scan.player_summary or 'PC files'))
            if not self.confirm_replace(prompt, ('wifi', existing, prompt, self.st.save_scan)):
                return False
        job, cancel = self.start_copy()
        key = self.copy_key()
        resume, scan, console, product = self.ftp_resume, self.st.save_scan, self.addr.get(), self.st.product
        self.app.work(lambda: ftp_saves.copy(console, product, scan, existing, overwrite=bool(existing.files),
                        cancelled=cancel.is_set, resume=resume,
                        progress=lambda event: self.app.call(self.show_progress, job,
                            ('Backing up 3DS saves' if '/backup-' in event.path else 'Copying PC saves'),
                            event.done, event.total)),
                      lambda result: self.finish_copy(job, key, result), errors=True)
        return False

    def busy(self):
        return self.ftp_running

    def auto(self):
        ap = self.app.opts.autopilot or {}
        if ap.get('import_saves'):
            return ('fail', 'Automatic save import is not supported. Open the builder and choose how to copy the PC saves.')
        if ap.get("save_dir") and self.custom_dir != os.path.normpath(ap["save_dir"]):
            self.custom_dir = os.path.normpath(ap["save_dir"])
            self.scan()
        if "import_saves" in ap:
            self.choice.set("skip")
        return "next"


class DonePage(Page):
    title = "What to do now"

    def __init__(self, app):
        super().__init__(app)
        self.headline = label(self.frame, "", bold=True, scale=2)
        self.summary_label = label(self.frame, "")
        self.steps_label = label(self.frame, "")
        self.buttons_row = ttk.Frame(self.frame)
        self.buttons_row.pack(anchor="w", pady=(8, 0))
        self.cleaning = False
        self.cleanup_only = False
        self.cleanup_plan = None
        self.cleanup_box = ttk.Frame(self.frame)
        self.cleanup_box.pack(fill='x', pady=(12, 0))
        label(self.cleanup_box, 'Free up space on this computer (optional)', bold=True)
        self.cleanup_choices = {}
        self.cleanup_checks = {}
        for key, title in [('build', 'Build files and caches'), ('downloads', 'Downloaded installers'),
                           ('tools', '3DS compiler and tools this builder installed')]:
            value = tk.BooleanVar(value=False)
            widget = checkbutton(self.cleanup_box, text=title, variable=value)
            widget.pack(anchor='w')
            self.cleanup_choices[key] = value
            self.cleanup_checks[key] = (widget, title)
            value.trace_add('write', lambda *_: self.app.update_nav())
        label(self.cleanup_box, 'Removing build files makes the next build slower. Removed tools are installed again from the compiler page when needed.')
        self.cleanup_help = label(self.cleanup_box, '')
        self.cleanup_notice = label(self.cleanup_box, '')
        self.cleanup_preview = button(self.cleanup_box, text='Show files to delete', command=self.preview_cleanup)
        self.cleanup_preview.pack(anchor='w')

    def on_show(self):
        self.cleanup_help.config(text=(
            'Choose Clean up to remove the ticked items, or Done to return to the first screen. '
            'Cleanup shows a report before returning.' if self.cleanup_only else
            'Clean up and finish removes the ticked items, then shows a report before closing. '
            'Finish closes without cleanup. To return later, choose Clean up previous build files on the first screen.'))
        if self.cleanup_only:
            self.headline.config(text="Clean up previous build files")
            self.summary_label.config(text="No build is needed. Choose what to remove below. Your game, saves and finished builds are kept.")
            self.steps_label.config(text="")
            for widget in self.buttons_row.winfo_children():
                widget.destroy()
            for value in self.cleanup_choices.values():
                value.set(False)
            self.refresh_cleanup()
            return
        p = self.st.product
        self.headline.config(text="%s is ready." % p.title)
        summary = [
            "Built: %s (%s)" % (
                " and ".join(SHORT_TARGET.get(t, t) for t in self.st.targets),
                ", ".join("." + o for o in self.st.outputs),
            )
        ]
        if self.st.sd_root:
            summary.append("SD card: %s" % self.st.sd_root)
        elif self.st.deliver_how == "wifi":
            summary.append("Sent over Wi-Fi.")
        else:
            summary.append("Output folder: %s" % (self.st.files_root or os.path.join(self.app.opts.work, "out")))
        res = self.st.current_save_result()
        if res and res.copied:
            summary.append('PC saves: copied %s.' % (
                'over Wi-Fi to %s' % self.st.save_destination if self.st.save_how == 'wifi'
                else 'to the SD card at %s' % self.st.save_destination))
            # The count and folder are in the steps below (deliver.instructions).
            if res.backup_dir:
                summary.append("Previous 3DS saves backed up to: %s" % res.backup_dir)
        else:
            summary.append("PC saves: not copied.")
        if self.st.texture_result is not None:
            summary.append(self.st.texture_result.message)
        if self.st.sd_root and deliver.dsp_missing(self.st.sd_root):
            summary.append(deliver.DSP_MESSAGE)
        self.summary_label.config(text="\n".join(summary))
        self.steps_label.config(text=deliver.instructions(p, self.st.targets, self.st.outputs, self.st.sd_root,
                                                          saves_result=res, delivery=self.st.deliver_how)
                                .removeprefix('What to do now:\n'))
        for w in self.buttons_row.winfo_children():
            w.destroy()
        if self.st.sd_root:
            button(self.buttons_row, text="Open SD card",
                   command=lambda r=self.st.sd_root: open_folder(r)).pack(side="left", padx=(0, 8))
        button(self.buttons_row, text="Open output folder",
               command=lambda: open_folder(self.st.files_root if self.st.deliver_how == "files"
                                            else os.path.join(self.app.opts.work, "out"))).pack(side="left")
        for value in self.cleanup_choices.values():
            value.set(False)
        self.refresh_cleanup()

    def selected_cleanup(self):
        return [key for key, choice in self.cleanup_choices.items() if choice.get()]

    def finish_label(self):
        if self.cleanup_only:
            return 'Clean up' if self.selected_cleanup() else 'Done'
        return 'Clean up and finish' if self.selected_cleanup() else 'Finish'

    def can_finish(self):
        return not self.cleaning and (not self.selected_cleanup() or self.cleanup_plan is not None)

    def finish(self):
        if self.cleaning:
            return False
        selected = self.selected_cleanup()
        if not selected:
            if self.cleanup_only:
                self.close_cleanup()
                return False
            return True
        if self.cleanup_plan is None:
            return False  # Keep the counting/ownership error on screen.
        self.run_cleanup(self.cleanup_plan, selected, finish=True)
        return False  # The report's Close button exits after the user reviews it.

    def refresh_cleanup(self):
        self.cleaning = True
        self.cleanup_plan = None
        self.cleanup_preview.state(['disabled'])
        self.cleanup_notice.config(text='Counting files...')
        def after(result):
            self.cleaning = False
            if isinstance(result, Exception):
                self.cleanup_notice.config(text=str(result))
                self.cleanup_plan = None
            else:
                self.cleanup_plan = result
                for key, (widget, title) in self.cleanup_checks.items():
                    widget.config(text='%s (%.1f MB)' % (title, result.size(key)/1e6))
                self.cleanup_notice.config(text='Your game, saves and finished builds are kept. '
                                           'Use Show files to delete for a summary.')
                self.cleanup_preview.state(['!disabled'])
            self.app.update_nav()
        self.app.work(lambda: cleanup.plan(self.app.opts.work, protected=(self.st.folder, self.st.sd_root,
                                                                         self.st.files_root, self.app.opts.folder)), after, errors=True)

    def preview_cleanup(self):
        selected = [key for key, choice in self.cleanup_choices.items() if choice.get()]
        plan = getattr(self, 'cleanup_plan', None)
        if plan is None:
            return  # Keep the counting error, rather than asking for a checkbox.
        if not selected:
            self.cleanup_notice.config(text='Tick what to remove first.')
            return
        self.app.prompt('Files to delete',
                        'Selected files will be deleted. Large groups are shown by folder and file count. Your game, saves and finished builds are kept.',
                        [('Cancel', None, False), ('Delete these files', lambda: self.run_cleanup(plan, selected), True)],
                        details=plan.preview(selected), cancel='Cancel', focus='Cancel')

    def run_cleanup(self, plan, selected, *, finish=False):
        if self.cleaning:
            return
        self.cleaning = True
        self.cleanup_preview.state(['disabled'])
        for widget, _ in self.cleanup_checks.values():
            widget.state(['disabled'])
        self.cleanup_notice.config(text='Removing the selected files...')
        self.app.update_nav()
        def after(result):
            self.cleaning = False
            for widget, _ in self.cleanup_checks.values():
                widget.state(['!disabled'])
            self.cleanup_preview.state(['!disabled'])
            if isinstance(result, Exception):
                self.cleanup_notice.config(text=str(result))
            else:
                deleted, kept = result
                self.cleanup_notice.config(text='Removed %d files. Your game, saves and finished builds are kept.'
                                           % len(deleted))
                for value in self.cleanup_choices.values():
                    value.set(False)
                # Invalidate the consumed list; another deletion requires a new preview.
                self.cleanup_plan = None
                self.cleanup_preview.state(['disabled'])
                if finish:
                    self.report_cleanup(deleted, kept)
                else:
                    self.refresh_cleanup()
            self.app.update_nav()
        self.app.work(lambda: cleanup.remove(plan, selected), after, errors=True)

    def report_cleanup(self, deleted, kept):
        report = ['Removed files:'] + (list(deleted) or ['None.'])
        if kept:
            report += ['', 'Kept files:'] + list(kept)
        self.app.prompt('Cleanup report', 'Removed %d files. Review the list, then click Close to finish.' % len(deleted),
                        [('Close', self.close_cleanup, True)], details='\n'.join(report), cancel='Close')

    def close_cleanup(self):
        if self.cleanup_only:
            self.cleanup_only = False
            self.app.show(0)
        else:
            self.app.root.destroy()

    def busy(self):
        return self.cleaning

    def auto(self):
        return "done"


def open_folder(path):
    os.makedirs(path, exist_ok=True)
    if sys.platform == "win32":
        os.startfile(path)  # noqa: S606
    else:
        subprocess.Popen(["open" if sys.platform == "darwin" else "xdg-open", path])


# ---------------------------------------------------------------------------------------------------------------------

class App:
    PAGES = (WelcomePage, FindPage, VerifyPage, ToolchainPage, OptionsPage, BuildPage, DeliverPage, SavesPage, DonePage)

    def __init__(self, root, opts: Options):
        self.root, self.opts = root, opts
        self.state = State(opts.game or "undertale")
        self.q = queue.Queue()
        self.update_checker = updates.Checker(opts.work, self.call)
        self.exit_code = 0
        root.configure(bg="#000000")
        boring = preferences.load(opts.work)["boring"] if opts.boring is None else opts.boring
        self.theme = Theme(root, boring=boring)
        root._ut_theme = self.theme
        # Load available player assets before constructing the first page, just as Find does later.
        folder = opts.folder
        if not folder:
            try:
                copies = discover.find_copies(self.state.game, opts.probes)
            except (OSError, ValueError):
                copies = []  # Find still offers explicit discovery and a folder chooser.
            folder = copies[0].folder if copies else None
        self.theme.load_from_game_dir(folder)
        root.title(self.state.product.title + ' 3DS Builder')
        mask = self.theme.soul_mask
        self._window_icon = tk.PhotoImage(master=root, width=len(mask[0]) * 2, height=len(mask) * 2)
        for y, row in enumerate(mask):
            for x, on in enumerate(row):
                if on:
                    self._window_icon.put('#ff0000', to=(x * 2, y * 2, x * 2 + 2, y * 2 + 2))
        root.iconphoto(True, self._window_icon)
        # Sizes are for 96 DPI. Where Tk renders larger text (a DPI-aware Linux or macOS desktop), the window grows
        # with it, up to the screen.
        grow = max(1.0, float(root.tk.call("tk", "scaling")) / (96 / 72))
        screen_w, screen_h = root.winfo_screenwidth() - 40, root.winfo_screenheight() - 80
        w, h = min(screen_w, round(760 * grow)), min(screen_h, round(680 * grow))
        root.geometry("%dx%d" % (w, h))
        root.minsize(min(w, round(680 * grow)), min(h, round(560 * grow)))
        head = ttk.Frame(root, padding=(16, 12, 16, 4))
        head.pack(fill="x")
        self.heading = PixelLabel(head, text="", bold=True, scale=2, color=WHITE)
        self.heading.pack(side="left")
        self.stepno = PixelLabel(head, text="", bold=True, scale=1, color=YELLOW)
        self.stepno.pack(side="right")
        # Created before the nav bar so that Tab reaches the page first, then About, Boring mode, Back and Next.
        self.body = tk.Frame(root, bg="#000000", padx=16, pady=6)
        self.body._ut_theme = self.theme
        self.pages = [P(self) for P in self.PAGES]
        self.index = 0
        self.keys = KeyWatch(root)  # a held Space or Enter never carries into a button that appears under it
        self.overlay = None         # the Prompt shown in place of the page, if any
        self._prompts = []          # prompts waiting for the one shown to close
        self._quitting = False      # Quit is stopping the build; the window closes when it is done (TK-D9)
        self._focus_before = None   # what had the keyboard before the first prompt opened
        nav = ttk.Frame(root, padding=(16, 8, 16, 12))
        nav.pack(side="bottom", fill="x")
        self.body.pack(fill="both", expand=True)
        self.about_btn = button(nav, text="About", command=self.about)
        self.about_btn.pack(side="left")
        self.boring = tk.BooleanVar(root, value=boring)
        self.boring_toggle = checkbutton(nav, text="Boring mode", variable=self.boring,
                                        command=self.toggle_boring)
        self.boring_toggle.pack(side="left", padx=(12, 0))
        self.back_btn = button(nav, text="< Back", command=self.back)
        self.next_btn = button(nav, text="Next >", command=self.next, primary=True)
        self.next_btn.pack(side="right")
        self.back_btn.pack(side="right", padx=8)
        self.theme.apply()
        for seq in ("<MouseWheel>", "<Shift-MouseWheel>", "<Button-4>", "<Button-5>",
                    "<Shift-Button-4>", "<Shift-Button-5>"):
            root.bind_all(seq, self._wheel, add="+")
        # Mouse focus must not move a pressed button before its release event.
        root.bind_all("<KeyRelease-Tab>", self._focused, add="+")
        try:
            root.bind_all("<KeyRelease-ISO_Left_Tab>", self._focused, add="+")
        except tk.TclError:
            pass  # Tk 8.5 off X11 (Apple's own Python) has no such key: Shift+Tab is Tab there, bound above.
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.bind("<Escape>", self._escape)
        self.show(0)
        root.after(100, self.poll)
        if opts.autopilot is not None:
            root.after(600, self.auto_tick)
        root.after_idle(self._mapped_theme)
        root.after_idle(lambda: _bring_to_front(root))

    def _mapped_theme(self):
        self.theme.apply()
        self.page.fit()

    def _wheel(self, event):
        """The mouse wheel scrolls a page that is taller than the window, except over a widget that scrolls itself."""
        # Aqua/Tk 9 may omit useful root pointer coordinates for a trackpad
        # event. Its receiving widget is authoritative; hit testing is fallback.
        widget = event.widget
        if not isinstance(widget, tk.Misc):
            try:
                widget = self.root.winfo_containing(event.x_root, event.y_root)
            except (tk.TclError, KeyError):
                return
        page = self.overlay or self.page
        if widget is None or not str(widget).startswith(str(page.box) + "."):
            return
        if isinstance(widget, (ttk.Scrollbar, ttk.Combobox)):
            return
        axis = getattr(event, '_scroll_axis', 'x' if getattr(event, 'state', 0) & 1 else 'y')
        target = widget if isinstance(widget, (tk.Text, ttk.Treeview, tk.Listbox)) else page.canvas
        view = getattr(target, axis + 'view')
        if view() == (0.0, 1.0):
            target = page.canvas
        if target is page.canvas and (axis == 'x' or not page.scrollable()):
            return 'break'
        if hasattr(event, '_scroll_amount'):
            # Canvas units are 20 pixels; native text/table units are rows.
            amount = event._scroll_amount / (20 if target is page.canvas else 16)
        else:
            event._tk9_wheel = int(str(self.root.tk.call('package', 'require', 'Tk')).split('.')[0]) >= 9
            amount = wheel_units(event, sys.platform)
        key = '_wheel_remainder_' + axis
        remainder = getattr(target, key, 0.0) + amount
        units = int(remainder)
        setattr(target, key, remainder - units)
        if units:
            getattr(target, axis + 'view_scroll')(units, "units")
        return "break"

    def _focused(self, event):
        widget = event.widget
        page = self.overlay or self.page
        if isinstance(widget, tk.Misc) and str(widget).startswith(str(page.frame) + "."):
            self.root.after_idle(lambda: widget.winfo_exists() and page.show_widget(widget))

    def toggle_boring(self):
        self.theme.boring = self.boring.get()
        self.theme.apply()
        preferences.save(self.opts.work, self.theme.boring)

    @property
    def page(self):
        return self.pages[self.index]

    def reset(self, game):
        self.pages[0].update_feedback.config(text='')
        self.state = State(game)
        self.root.title(self.state.product.title + ' 3DS Builder')
        self.pages[1].searched_for = None

    def show(self, i):
        if self.state.folder:
            self.theme.load_from_game_dir(self.state.folder)
        self.page.box.pack_forget()
        self.index = i
        self.show_heading()
        if self.overlay is None:    # A page that moves on by itself stays behind a prompt that is open.
            self.page.box.pack(fill="both", expand=True)
        self.page.canvas.yview_moveto(0)
        stop = (self.opts.autopilot or {}).get("stop_at")
        if stop and stop == self.page.title:
            self.update_nav()
            print(self.summary() + "\n\nAUTOPILOT: stopped at %s as planned" % stop, flush=True)
            self.root.after(300, self.root.destroy)
            self.opts.autopilot = None
            return
        self.page.on_show()
        self.update_nav()
        if self.overlay is None:
            self.keys.carry()

    def show_heading(self):
        if self.overlay is not None:
            self.heading.config(text="* " + self.overlay.title)
            self.stepno.config(text="")
            return
        cleanup_only = isinstance(self.page, DonePage) and self.page.cleanup_only
        self.heading.config(text="* Clean up previous build files" if cleanup_only else "* " + self.page.title)
        self.stepno.config(text="" if cleanup_only else "Step %d of %d" % (self.index + 1, len(self.pages)))

    def prompt(self, title, message, choices, details=None, cancel=None, focus=None, collapse_details=False):
        """Ask or tell something inside this window (Prompt). One shows at a time; later ones wait their turn."""
        self._prompts.append((title, message, choices, details, cancel, focus, collapse_details))
        if self.overlay is None:
            self._next_prompt()

    def ask(self, title, message, yes, no=None, yes_text="Yes", no_text="No", safe=True):
        """A yes/no question. safe: the keyboard starts on No, for anything that deletes, replaces or stops."""
        self.prompt(title, message, [(no_text, no, False), (yes_text, yes, True)], cancel=no_text,
                    focus=no_text if safe else yes_text)

    def inform(self, title, message, details=None):
        self.prompt(title, message, [("OK", None, True)], details=details, cancel="OK")

    def _next_prompt(self):
        title, message, choices, details, cancel, focus, collapse_details = self._prompts.pop(0)
        if self.overlay is None and self._focus_before is None:
            self._focus_before = self.root.focus_lastfor()   # this window's keyboard, even while it is not active
        self.page.box.pack_forget()
        self.overlay = Prompt(self, title, message, choices, details=details, cancel=cancel, focus=focus,
                              collapse_details=collapse_details)
        self.overlay.box.pack(fill="both", expand=True)
        self.theme.apply()
        self.show_heading()
        self.update_nav()
        self.keys.hand(self.overlay.first)

    def _escape(self, _event=None):
        """Escape makes the open prompt's cancel choice; with no prompt open it does nothing."""
        if self.overlay is not None:
            self.overlay.choose(self.overlay.cancel)

    def close_prompt(self, prompt):
        if prompt is not self.overlay:
            return
        prompt.box.destroy()
        self.overlay = None
        if self._prompts:
            self._next_prompt()
            return
        self.page.box.pack(fill="both", expand=True)
        self.show_heading()
        self.update_nav()
        # Back to what had the keyboard before, as a system dialog does; never straight onto the footer's action.
        before, self._focus_before = self._focus_before, None
        self.keys.hand(None if before is self.next_btn else before)

    def update_nav(self):
        def enable(widget, enabled):
            # Setting an unchanged UtButton state regenerates its bitmap and forces another Tk layout.
            if widget.instate(['disabled']) == enabled:
                widget.state(['!disabled'] if enabled else ['disabled'])

        if self.overlay is not None:
            for control in (self.back_btn, self.next_btn, self.about_btn):
                enable(control, False)
            return
        enable(self.about_btn, True)
        last = self.index == len(self.pages) - 1
        text = self.page.finish_label() if last else 'Next >'
        if self.next_btn.cget('text') != text:
            self.next_btn.config(text=text)
        ok = self.page.can_next() and not self.page.busy()
        if last:
            ok = self.page.can_finish()
        enable(self.next_btn, ok)
        enable(self.back_btn, self.index > 0 and not self.page.busy())

    def next(self):
        if isinstance(getattr(self, "overlay", None), Prompt):   # Next waits while a prompt is open.
            return
        if self.index == len(self.pages) - 1:
            if self.page.busy():
                return
            if self.page.finish():
                self.root.destroy()
            return
        if self.page.can_next() and not self.page.busy():
            if not self.page.on_next():
                self.update_nav()
                return
            self.show(self.index + 1)

    def open_cleanup(self):
        self.pages[-1].cleanup_only = True
        self.show(len(self.pages)-1)

    def back(self):
        if self.index > 0 and not self.page.busy() and self.overlay is None:
            if isinstance(self.page, DonePage) and self.page.cleanup_only:
                self.page.cleanup_only = False
                self.show(0)
            else:
                self.show(self.index - 1)

    def about(self):
        app = products.PRODUCTS[self.state.game].app_name
        try:
            source = updates.repository(self.state.game) + '/releases'
        except Exception:
            source = ''
        if self.overlay is None:
            self.inform("About", ABOUT.format(app=app, source=source, holder=port_holder() or ABOUT_NO_HOLDER))

    def check_updates(self, manual=False):
        if self.pages[0].check_updates.get():
            if manual:
                self.pages[0].update_feedback.config(text='Checking...')
            accepted = self.update_checker.request(self.state.game,
                lambda game, result: self._release_notice(game, result, manual=manual), manual=manual)
            if manual and not accepted:
                self.pages[0].update_feedback.config(text="Couldn't check")

    def _release_notice(self, game, result, manual=False):
        if game == self.state.game and self.pages[0].check_updates.get():
            if result and result.notice:
                self.pages[0].release_notice(result.notice)
            if manual:
                text = ("Couldn't check" if result is None else
                        'v%s is out' % result.notice.version if result.notice else
                        'Up to date (v%s)' % result.current.removeprefix('v'))
                self.pages[0].update_feedback.config(text=text)

    def close(self):
        if self.overlay is not None:     # The window's close button makes the prompt's cancel choice.
            self.overlay.choose(self.overlay.cancel)
            return
        if self.page.busy() and self.index != 5:
            self.inform('Please wait', 'This step is still running. Wait for it to finish before closing the builder.')
            return
        b = self.pages[5]
        if self._quitting:
            return                      # already stopping the build to quit
        if b.running:
            self.ask("Quit", "Stop the build and quit?", lambda: self.quit_build(b))
            return
        # An outstanding SD scan is a daemon reader: closing abandons its queued result. The worker calls
        # only App.call (queue.put), and no poll runs after destroy, so it never touches a destroyed widget.
        self.root.destroy()

    def quit_build(self, page):
        """Stop the build and confirm it before the window goes (TK-D5, D9). The confirmation can take up to the
        stop bound, so it runs on its own thread and the window keeps drawing; a stop that could not be confirmed
        is said in the window before it closes."""
        self._quitting = True
        result = []
        stopper = threading.Thread(target=lambda: result.append(stop_build_for_exit(page)), name='quit-stop',
                                   daemon=True)
        stopper.start()
        page.intro.config(text="Stopping the build...")

        def wait():
            if stopper.is_alive():
                self.root.after(100, wait)
                return
            failure = result[0] if result else "stopping it failed"
            if failure:
                self.prompt("Quit", "The build could not be stopped (%s).\n"
                            "Some build tools may still be running." % failure,
                            [("OK", self.root.destroy, True)], cancel="OK")
                return
            self.root.destroy()
        wait()

    # Worker threads hand results to the Tk thread through this queue.
    def call(self, fn, *args):
        self.q.put((fn, args))

    def work(self, job, done, errors=False):
        def run():
            try:
                res = job()
            except Exception as e:  # noqa: BLE001
                if not errors:
                    traceback.print_exc()
                res = e
            self.call(done, res)
        threading.Thread(target=run, daemon=True).start()

    def poll(self):
        # A compiler/transfer can outrun Tk while this PC is busy. Always hand control back to the event
        # loop, including while a producer is still filling the queue, so Windows can pump its messages.
        deadline = time.monotonic() + 0.020
        try:
            for _ in range(100):
                fn, args = self.q.get_nowait()
                if len(args) == 1 and isinstance(args[0], dict) and getattr(fn, "__name__", "") in ("config",
                                                                                                     "configure"):
                    fn(**args[0])
                else:
                    fn(*args)
                if time.monotonic() >= deadline:
                    break
        except queue.Empty:
            pass
        if isinstance(self.page, BuildPage) and self.page.running and self.page.build is not None:
            self.page.progress(self.page.build.progress)
        (self.overlay or self.page).fit()
        self.root.after(1 if not self.q.empty() else 50, self.poll)

    # Autopilot: drives the real pages with a plan and prints what each page shows.
    def summary(self):
        lines = ["", "=== Page %d of %d: %s ===" % (self.index + 1, len(self.pages), self.page.title)]
        lines += text_of(self.page.frame)
        lines.append("<nav: Back %s, %s %s>" % ("on" if self.back_btn.instate(["!disabled"]) else "off",
                                                 self.next_btn.cget("text"),
                                                 "on" if self.next_btn.instate(["!disabled"]) else "off"))
        return "\n".join(lines)

    def auto_tick(self):
        try:
            r = self.page.auto()
        except Exception as e:  # noqa: BLE001
            traceback.print_exc()
            r = ("fail", repr(e))
        if r == "wait":
            self.root.after(500, self.auto_tick)
            return
        self.update_nav()
        print(self.summary(), flush=True)
        if r == "next":
            before = self.index
            self.next()
            if self.index == before:
                if isinstance(self.page, DeliverPage) and self.page.checking_files:
                    self.root.after(500, self.auto_tick)
                    return  # The checked Next callback will advance when its worker returns.
                status = getattr(self.page, "status", None)
                why = status.cget("text") if status is not None else "could not advance"
                print("\nAUTOPILOT: FAILED on %s: %s" % (self.page.title, why), flush=True)
                self.exit_code = 1
                self.root.after(500, self.root.destroy)
                return
            self.root.after(800, self.auto_tick)
        elif r == "done":
            print("\nAUTOPILOT: finished all %d pages" % len(self.pages), flush=True)
            self.root.after(500, self.root.destroy)
        else:
            print("\nAUTOPILOT: FAILED on %s: %s" % (self.page.title, r[1]), flush=True)
            self.exit_code = 1
            self.root.after(500, self.root.destroy)


def _bring_to_front(root):
    """Make startup visible above the launching terminal, then release the temporary topmost flag."""
    try:
        root.lift()
        root.attributes("-topmost", True)
        root.focus_force()
    finally:
        root.after_idle(lambda: root.attributes("-topmost", False))


WORKER_EXIT_SECONDS = 30    # how long an exit waits for a stopped build's worker to end its current step (TK-ST2)


def _hard_exit(code):
    os._exit(code)


def stop_build_for_exit(page):
    """Stop a build page's worker for any exit. Returns a warning, or None when nothing can still be running."""
    try:
        return page.stop_for_quit()
    except Exception as e:  # noqa: BLE001 (an error while stopping is not a confirmation)
        traceback.print_exc()
        return "stopping it failed (%s: %s)" % (type(e).__name__, e)


def run(opts: Options):
    # One builder window per work folder (AUD-15): a second one would replace the first one's sources and outputs.
    try:
        lease = pipeline.claim_work(opts.work, devkitpro=opts.devkitpro)
    except pipeline.WorkLockError as exc:
        print(exc.window, file=sys.stderr, flush=True)
        if opts.autopilot is None:
            root = tk.Tk()
            root.withdraw()
            title = "Builder already open" if isinstance(exc, pipeline.WorkBusy) else "Builder can't start"
            messagebox.showerror(title, exc.window, parent=root)
            root.destroy()
        return 1
    app = None
    try:
        root = tk.Tk()
        app = App(root, opts)
        root.mainloop()
        return app.exit_code
    finally:
        # Every exit, including one that skips close() (autopilot, a direct destroy) or an error out of the main
        # loop, stops a running build before the work folder's lease goes (TK-D9): a second builder must not take
        # the folder while ours still writes. While the worker is alive the lease is never released (TK-ST2): the
        # process ends instead, and the OS drops the lock only when the worker has gone with it.
        if _worker_ended_for_exit(app):
            lease.release()
        else:
            print("The builder is closing while the build is still finishing a step; it will be stopped with the "
                  "builder.", file=sys.stderr, flush=True)
            _hard_exit(1)


def _worker_ended_for_exit(app):
    """Stop the build page's worker, if one is running, and say whether it has ended (TK-D9, TK-ST2)."""
    pages = getattr(app, 'pages', None)
    page = pages[5] if isinstance(pages, list) and len(pages) > 5 else None
    done = getattr(page, 'worker_done', None)
    if done is None or done.is_set():
        return True
    failure = stop_build_for_exit(page)
    if failure:
        print("The build could not be stopped (%s). Some build tools may still be running." % failure,
              file=sys.stderr, flush=True)
    return done.is_set()
