"""The builder's window: one wizard, nine pages, Back and Next (docs/release/builder.md, "Wizard"). Plain tkinter.

UNDERTALE theme:
- Black background (#000000) and white pixel-border boxes (#ffffff).
- Pixel bitmap font:
  (a) Before the game copy is found or if its assets are unreadable: standard platform widgets and system font.
  (b) Once the player's copy is found on disk, loads the game's own bitmap font (fnt_maintext) and red SOUL sprite
      (spr_heart) from data.win, game.ios or game.unx at runtime (strictly read-only), falling back to (a).
      Boring mode always uses (a).
- Red SOUL (#ff0000) selection marker on radio/check/button controls and progress marker on progress bars.
- UNDERTALE-style orange (#ff7f27) / yellow (#ffff00) pixel-bordered buttons.
"""
from __future__ import annotations

import os
import queue
import struct
import subprocess
import sys
import threading
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
import toolchain
from processes import hidden_options

DISCLAIMER = "Unofficial fan project. Not affiliated with Toby Fox, Fangamer, YoYo Games, or Nintendo."
WHAT = 'Makes a 3DS version of your own PC game and puts it on your 3DS by SD card or Wi-Fi.'
TERMS = "Store terms (Steam, GOG) restrict modifying or converting the game. Building is your decision."
ABOUT = ("{app} builder\n\nFree software under the GNU AGPL-3.0. Source ZIP:\n{source}\n\n"
         "The ownership check is a deterrent, not a lock: it checks that the game files match an official release "
         "and that a store installed them.\n\n" + DISCLAIMER)
SOURCE_URL = "https://github.com/ethancawse/{app}/releases"
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
        self.glyphs = _fallback_glyphs()
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
        # Before assets are available, use the standard look rather than substitute game-like art/fonts.
        return self.boring or not self.runtime_loaded

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
            style.configure("Treeview", rowheight=max(20, tkfont.nametofont("TkDefaultFont", self.root)
                                                      .metrics("linespace") + 6))
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
        w = max(4, max(self.measure_line(line, scale) for line in lines) + 2)
        h = max(4, len(lines) * lh)
        buf = bytearray(w * h * 3)
        for i, line in enumerate(lines):
            self.draw_text_buf(buf, w, h, 0, i * lh + 1, line, color=color, scale=scale)
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
                self.bind(seq, lambda _e: webbrowser.open(self._url))
            self.bind("<FocusIn>", lambda _e: self._focus(True))
            self.bind("<FocusOut>", lambda _e: self._focus(False))
        self._join_page(parent)
        if self._theme is not None:
            self._theme.register(self)
            self._render()

    def _focus(self, on):
        if self._url and not self._bold:
            self._color = YELLOW if on else ORANGE
            self._render()

    def _link_font(self):
        font = getattr(self._theme, "_link_font", None)
        if font is None:
            font = tkfont.nametofont("TkDefaultFont", self._theme.root).copy()
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
    """A ttk.Progressbar subclass that draws a white pixel-bordered bar with the red SOUL as its progress marker."""

    def __init__(self, parent, mode="determinate", length=500, maximum=100, **kw):
        self._theme = _find_theme(parent)
        self._length = length
        self._max = maximum
        self._mode = mode
        self._val = float(kw.get("value", 0.0))
        self._step = 0
        self._animating = False
        self._img = None
        super().__init__(parent, mode=mode, length=length, maximum=maximum, **kw)
        self._overlay = tk.Label(self, text="", bd=0, highlightthickness=0, bg="#000000")
        self._overlay._lint_skip = True
        self._overlay.place(x=0, y=0, relwidth=1, relheight=1)
        # The picture follows the bar's real size (it may be stretched to the page width).
        self.bind("<Configure>", self._resized, add="+")
        if self._theme is not None:
            self._theme.register(self)
            self._render()

    def _resized(self, event):
        if (event.width, event.height) != (self._length, getattr(self, "_height", 0)):
            self._length, self._height = event.width, event.height
            self._render()

    def _render(self):
        if self._theme is None:
            return
        if self._theme.native:
            self._overlay.place_forget()
            self._overlay.configure(image="")
            self._img = None
            return
        self._overlay.place(x=0, y=0, relwidth=1, relheight=1)
        self._img = self._theme.render_progressbar_image(
            self._length, max(18, getattr(self, "_height", 20)), self._val, self._max,
            indeterminate=(self._mode == "indeterminate" and self._animating), step=self._step,
        )
        self._overlay.configure(image=self._img)

    def refresh_theme(self):
        self._render()

    def start(self, interval=None):
        self._animating = True
        self._step += 1
        super().start(interval)
        self._render()
        if getattr(self, "_tick_id", None) is None:
            self._tick()

    def _tick(self):
        # ttk animates only its own (covered) bar; the SOUL moves here, a few times a second.
        self._tick_id = None
        if not self._animating or not self.winfo_exists():
            return
        if self._mode == "indeterminate" and self._theme is not None and not self._theme.native:
            self._step += 1
            self._render()
        self._tick_id = self.after(120, self._tick)

    def stop(self):
        self._animating = False
        super().stop()
        if getattr(self, "_tick_id", None) is not None:
            self.after_cancel(self._tick_id)
            self._tick_id = None
        self._render()

    def configure(self, cnf=None, **kw):
        if isinstance(cnf, dict):
            kw = {**cnf, **kw}
            cnf = None
        if not cnf and not kw:
            return super().configure()
        if "mode" in kw:
            self._mode = kw["mode"]
            if self._mode != "indeterminate":
                self._animating = False
        if "value" in kw:
            self._val = float(kw["value"])
        if "maximum" in kw:
            self._max = float(kw["maximum"])
        res = super().configure(cnf, **kw)
        self._render()
        return res

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
        self.delivered = False
        self.deliver_how = "sd"
        self.sd_root = None
        self.files_root = None
        self.ftp_console = None
        self.save_scan = None
        self.save_result = None
        self.saves_done = False

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


# ---------------------------------------------------------------------------------------------------------------------

class WelcomePage(Page):
    title = "Welcome"

    def __init__(self, app):
        super().__init__(app)
        label(self.frame, DISCLAIMER, bold=True)
        label(self.frame, WHAT)
        try:
            release = "Release %s. Old and New 3DS families. 30 fps." % pipeline.release_version()
        except pipeline.BuildFailed:
            release = "Source files need repair. Download and unpack the builder again."
        label(self.frame, release)
        label(self.frame, TERMS)
        self.game = tk.StringVar(value=app.state.game)
        if not app.opts.game:
            label(self.frame, "Game:", bold=True)
            for key, p in products.PRODUCTS.items():
                radiobutton(self.frame, text=p.title, value=key, variable=self.game,
                            command=self.changed).pack(anchor="w")

    def changed(self):
        if self.game.get() != self.st.game:
            self.app.reset(self.game.get())

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
            self.tree.heading(c, text=c.capitalize())
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

    def on_show(self):
        self.intro.config(text="Select your %s install." % self.st.product.title)
        if self.searched_for != self.st.game:
            if self.app.opts.folder:
                self.searched_for = self.st.game
                self.add_manual(self.app.opts.folder)
            else:
                self.search()

    def search(self):
        self.searching = True
        self.searched_for = self.st.game
        self.tree.delete(*self.tree.get_children())
        self.status.config(text="Searching...")
        game = self.st.game
        self.app.work(lambda: discover.find_copies(game, self.app.opts.probes), self.found)

    def found(self, copies):
        self.searching = False
        self.tree.delete(*self.tree.get_children())
        for i, c in enumerate(copies):
            self.tree.insert("", "end", iid=str(i), values=(c.store, c.folder, c.version + (" - " + c.note if c.note
                                                                                             else "")))
        if copies:
            self.tree.selection_set("0")
            self.status.config(text="Found %d %s." % (len(copies), "copy" if len(copies) == 1 else "copies"))
        else:
            p = self.st.product
            stores = "Steam or GOG" if p.game.gog_product else "Steam"
            self.status.config(text="%s was not found.\nInstall it in %s and click Search again, or Choose folder..."
                                    % (p.title, stores))
        self.picked()

    def picked(self):
        sel = self.tree.selection()
        if sel:
            new = self.tree.item(sel[0], "values")[1]
            if new != self.st.folder:
                self.st.folder = new
                self.st.verdict = None
                self.app.theme.load_from_game_dir(new)
        self.app.update_nav()

    def choose(self):
        d = filedialog.askdirectory(title="Select %s folder" % self.st.product.title)
        if d:
            self.add_manual(os.path.normpath(d))

    def add_manual(self, d):
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
        self.intro.config(text="Checking %s..." % self.st.folder)
        if self.st.verdict is None and not self.running:
            self.start()
        else:
            self.render()

    def start(self):
        self.running = True
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
        if verdict.accepted:
            self.st.parts = [p for p in verdict.parts if p != "root"] or None
        self.render()
        self.app.update_nav()

    def render(self):
        v = self.st.verdict
        if v is None:
            return
        self.bar.config(mode="determinate", value=100)
        self.result.config(text=("OK: " if v.accepted else "Refused: ") + v.message)
        self.details.config(text="\n".join(v.result.details))
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

    A failed step keeps its "Show command" and "Open download page" buttons, so the user can still do it by hand."""

    def __init__(self, page, parent, step, button_text="Install"):
        self.page, self.step = page, step
        dry = page.app.opts.toolchain_dry_run
        self.box = ttk.LabelFrame(parent, text=step.title, padding=6)
        self.box.pack(fill="x", pady=(0, 6))
        # Full consent remains in the confirmation and the adjacent Licences dialog.
        self.row = ttk.Frame(self.box)
        self.row.pack(fill="x")
        self.go = button(self.row, text="Show command" if dry else button_text,
                         command=lambda: page.run_step(self))
        self.go.pack(side="left", padx=(0, 8))
        self.show_btn = button(self.row, text="Show command", command=self.show_command)
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
            self.tree.heading(c, text={"ok": "", "item": "Item", "detail": "Found"}[c])
            self.tree.column(c, width=w, minwidth=60, stretch=(c == "detail"))
        self.tree.pack(fill="x", pady=(0, 8))
        self.tree.bind('<<ThemeChanged>>', lambda _e: self.fit_status())
        self.fit_status()
        self.result = label(self.frame, "")
        self.steps = ttk.Frame(self.frame)
        self.steps.pack(fill="x")
        self.extra = ttk.Frame(self.frame)
        self.extra.pack(fill="x")
        notice, links = toolchain.licence_notice(sys.platform)
        self.notice = label(self.frame, "Install or Update means you agree to the listed licences.")
        self.notice.pack_configure(before=self.steps)
        row = ttk.Frame(self.frame)
        row.pack(anchor="w", pady=(0, 8), before=self.steps)
        button(row, text="Licences", command=lambda: messagebox.showinfo(
            "Licences", notice + "\n\n" + "\n\n".join(name + "\n" + url for name, url in links))).pack(side="left")
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
        self.app.work(lambda: toolchain.detect(opts.tools_dir, roots=roots), self.detected)

    def detected(self, tc):
        self.detecting = False
        if isinstance(tc, Exception):
            self.result.config(text="Could not check the tools: %s" % tc)
            self.write(str(tc))
            self.show_log()
            self.app.update_nav()
            return
        self.st.tc = tc
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
        for i, (step, needed, waiting) in enumerate(toolchain.setup_inventory(tc, steps)):
            status = "Installed" if not needed else "Waiting for %s." % waiting if waiting else "Ready"
            self.tree.insert("", "end", iid=str(i), values=(status, step.title,
                             "Optional: .cia only" if step.key in ('makerom', 'bannertool') else ""))
            box = self.boxes[i]
            if step.key in self.failures:
                box.failed(self.failures[step.key])
            else:
                box._say(status)
        self.fit_status()
        self.update_buttons()
        self.app.update_nav()

    def fit_status(self):
        # Theme/font changes can arrive before detection has populated the rows.
        font = tkfont.Font(self.tree, font=ttk.Style(self.tree).lookup('Treeview', 'font') or 'TkDefaultFont')
        labels = ['Installed'] + [self.tree.item(row, 'values')[0] for row in self.tree.get_children()]
        width = max(110, max(font.measure(text) for text in labels) + 28)
        self.tree.column('ok', width=width, minwidth=width, stretch=False)

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

    def run_step(self, box):
        step = box.step
        dry = self.app.opts.toolchain_dry_run
        if self.running_step:
            return False
        ask = self.app.opts.confirm_install or messagebox.askokcancel
        if not dry and not ask(step.title, step.confirm_text()):
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

        def job():
            toolchain.run_step(step, log, dry, actions, stage=lambda kind, text: self.app.call(box.stage, kind, text),
                               work=self.app.opts.work, root=self.st.tc.root)
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
                self.result.config(text="%s did not finish. Show command explains how to do it by hand." % step.title)
            else:
                box.succeeded()
                if not dry:
                    self.result.config(text="Done: %s." % step.title)
            if not dry and not isinstance(result, Exception):
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
        if not self.run_step(self.update_box):
            self.update_box.box.destroy()
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
        self.cia_box = checkbutton(self.frame, text=".cia (HOME Menu, needs CFW)",
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
            self.cia_note.config(text="(.cia needs the .cia tools from the previous step.)")
        for w in self.extra.winfo_children():
            w.destroy()
        self.part_vars = {}
        if self.st.game == "deltarune" and self.st.verdict:
            import chapters
            states = chapters.inventory(self.st.folder)
            installed = [p for p, v in states.items() if p != 'root' and v[0] != 'missing']
            label(self.extra, 'Version: %s\nInstalled chapters: %s' %
                  (self.st.verdict.result.version or 'unknown', chapters.numbers(installed)), bold=True)
            for part in self.st.verdict.parts:
                if part == "root":
                    continue
                v = tk.BooleanVar(value=True)
                self.part_vars[part] = v
                checkbutton(self.extra, text="Chapter " + part[len("chapter")], variable=v,
                            command=self.changed).pack(anchor="w")
            self.h264 = tk.BooleanVar(value=False)
            checkbutton(self.extra, text="Download openh264 2.6.0 for intro videos",
                        variable=self.h264, command=self.changed).pack(anchor="w")
        self.changed()

    def changed(self):
        t = self.target.get()
        self.st.targets = ["n3ds", "o3ds"] if t == "both" else [t]
        self.st.outputs = [k for k, v in (("3dsx", self.v3dsx), ("cia", self.vcia)) if v.get()]
        if self.part_vars:
            self.st.parts = [p for p, v in self.part_vars.items() if v.get()]
        self.st.openh264 = bool(getattr(self, "h264", None) and self.h264.get())
        p = self.st.product
        disk = sum(p.disk_mb[x] for x in self.st.targets)
        if self.st.game == 'deltarune' and self.st.folder:
            import chapters
            disk += (chapters.stage_bytes(self.st.folder, self.st.parts) + (1 << 20) - 1) // (1 << 20)
        self.estimate.config(text="About %d MB in %s." % (disk + 30, self.app.opts.work))
        if self.st.game == 'deltarune':
            import chapters
            self.estimate.config(text=self.estimate.cget('text') + '\nSelected chapters: ' +
                                 chapters.numbers(self.st.parts or []))
        self.st.build_ok = False
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

    def on_show(self):
        self.intro.config(text="Building...")
        if not self.st.build_ok and not self.running:
            self.start()
        self.buttons()

    def buttons(self):
        self.cancel_btn.state(["!disabled"] if self.running else ["disabled"])
        self.retry_btn.state(["!disabled"] if (self.failed and not self.running) else ["disabled"])
        self.copy_btn.state(["!disabled"] if self.log_path else ["disabled"])

    def start(self):
        self.failed = None
        self.build = None
        self.running = True
        self.st.artifacts, self.st.records = [], []
        self.log.delete("1.0", "end")
        self.intro.config(text="Building...")
        self.status.config(text="")
        self.eta.config(text="")
        self.bar.config(value=0)
        self.buttons()
        self.app.update_nav()
        threading.Thread(target=self.worker, daemon=True).start()

    def worker(self):
        try:
            for i, target in enumerate(self.st.targets):
                self.target_index = i
                self.app.call(self.intro.config, {"text": "Building for %s..." % SHORT_TARGET.get(target, target)})
                b = pipeline.Build(self.st.product, self.st.tc, self.app.opts.work, target, self.st.folder,
                                   store=self.st.verdict.result.store,
                                   outputs=self.st.outputs, openh264=self.st.openh264, parts=self.st.parts,
                                   on_line=lambda s: self.app.call(self.line, s),
                                   on_progress=lambda p: self.app.call(self.progress, p))
                self.build = b
                self.log_path = b.log_path
                arts, record = b.run()
                self.st.artifacts += arts
                self.st.records.append(record)
            self.app.call(self.finished, None)
        except pipeline.BuildFailed as e:
            self.app.call(self.finished, e)
        except Exception as e:  # noqa: BLE001 (shown to the user, with the traceback in the log)
            traceback.print_exc()
            self.app.call(self.finished, pipeline.BuildFailed(pipeline.FAILED + " (%s: %s)" % (type(e).__name__, e),
                                                              self.log_path))

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
        if err is None:
            self.st.build_ok = True
            self.intro.config(text="Build complete.")
            self.bar.config(value=100)
            self.eta.config(text="100%")
            lines = ["Built: %s." % ", ".join("%s %s" % (SHORT_TARGET.get(r["target"], r["target"]),
                                                          _fmt_secs(r["seconds"]))
                                               for r in self.st.records)]
            for a in self.st.artifacts:
                lines.append("%s (%.1f MB)" % (a.path, a.size / 1e6))
            self.status.config(text="\n".join(lines))
        else:
            self.failed = err
            self.details.show()
            self.intro.config(text="Build stopped.")
            self.log_path = err.log_path or self.log_path
            self.status.config(text="%s\nLog: %s" % (err, self.log_path))
        self.buttons()
        self.app.update_nav()

    def cancel(self):
        if self.build and messagebox.askyesno("Cancel build", "Stop the build?"):
            self.build.cancel()

    def copy_log(self):
        if not self.log_path:
            return
        with open(self.log_path, encoding="utf-8", errors="replace") as f:
            text = pipeline.filter_log(f.read())
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


class DeliverPage(Page):
    """Each way of delivering has its own panel and its own status text; a progress bar shows only while copying."""
    title = "Put it on the 3DS"

    def __init__(self, app):
        super().__init__(app)
        self.how = tk.StringVar(value="sd")
        radiobutton(self.frame, text="Copy to SD card", value="sd", variable=self.how,
                    command=self.mode).pack(anchor="w")
        radiobutton(self.frame, text="Install over Wi-Fi", value="wifi",
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
        label(self.wifi, '3DS address (shown by ftpd):')
        self.addr = ttk.Combobox(self.wifi, width=30)
        self.addr.pack(anchor="w", pady=(0, 6))
        self.addr.set(preferences.load(app.opts.work).get('console', ''))
        label(self.wifi, "Start ftpd on the 3DS. Keep it open while the files are sent.")
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

    def on_show(self):
        self.refresh()
        self.mode()

    def set_status(self, how, text):
        """One status text per panel; only the visible panel's text is on screen."""
        self.texts[how] = text
        if how == self.how.get():
            self._show_status()

    def _show_status(self):
        how = self.how.get()
        for w in (self.bar, self.status):
            w.pack_forget()
        anchor = self.panels[how]
        if not anchor.winfo_manager():
            return  # mode() shows the panel, then calls this again
        if self.bar_used[how]:
            self.bar.pack(fill="x", pady=(4, 6), after=anchor)
            anchor = self.bar
        text = self.texts[how]
        self.status.config(text=text)
        if text:
            self.status.pack(anchor="w", pady=(0, 6), after=anchor)

    def show_progress(self, how, value):
        self.bar_used[how] = True
        self.bar.config(value=value)
        if how == self.how.get() and not self.bar.winfo_ismapped():
            self._show_status()

    def mode(self):
        for f in self.panels.values():
            f.pack_forget()
        how = self.how.get()
        if how != self.st.deliver_how:
            self.st.delivered = False
        self.st.deliver_how = how
        self.panels[how].pack(anchor="w", fill="x")
        if how == "files":
            ready = self.saved_key == self.files_key() and bool(self.st.files_root)
            self.steps.config(text=("Output: %s\nRead READ-ME-FIRST.txt before copying. Use Copy to SD card for a safe merge." % self.st.files_root
                                    if ready else "Save the build, game files and prepared textures in one folder."))
            self.open_files.state(["!disabled"] if ready else ["disabled"])
            self.st.sd_root = None
            self.st.delivered = ready
        self._show_status()
        self.app.update_nav()

    def files_key(self):
        return (self.st.game, self.st.folder, tuple(self.st.parts or ()),
                tuple((a.path, a.sha256) for a in self.st.artifacts))

    def save_files(self):
        if self.copying:
            return
        key = self.files_key()
        self.copying = True
        self.st.delivered = False
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
                self.saved_key = key
                self.st.files_root = getattr(res, 'folder', None) or res.root
                self.show_progress("files", 100)
                self.set_status("files", "Saved %d files (%.1f MB).\n%s" %
                                (res.files, res.bytes / 1e6, res.textures.message))
            self.mode()
        self.app.work(lambda: deliver.save_files(self.st.product, self.st.artifacts, self.st.folder,
                                                 self.app.opts.work, self.st.parts, prog), after, errors=True)

    def refresh(self):
        try:
            self.cards = deliver.list_sd_cards()
        except OSError:
            self.cards = []
        vals = ["%s  %s (%d MB free)" % (c.root, c.label, c.free // 10**6) for c in self.cards]
        if self.manual:
            vals.append(self.manual)
        self.card.config(values=vals)
        if vals:
            self.card.current(len(vals) - 1 if self.manual else 0)
            self.set_status("sd", "")
        else:
            self.card.set("")
            self.set_status("sd", "No 3DS SD card found.\nInsert your SD card and click Refresh, or Choose folder...")
        self.go.state(["!disabled"] if vals and not self.copying else ["disabled"])

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
        return v.split("  ")[0]

    def copy(self):
        if self.copying:
            return
        root = self.target_root()
        if not root:
            return
        self.copying = True
        self.go.state(["disabled"])
        self.set_status('sd', 'Preparing a list of files. Nothing is being copied to the card yet.')
        self.app.update_nav()
        def prepared(result):
            if isinstance(result, Exception):
                self.copying = False
                self.go.state(['!disabled'])
                self.set_status('sd', str(result))
                self.app.update_nav()
                return
            items, tool, text = result
            # Autopilot only writes to its explicit disposable card fixture.
            if self.app.opts.autopilot is not None:
                self._copy_approved(root, items, tool)
                return
            window = tk.Toplevel(self.frame)
            window.title('Files for this SD card')
            window.geometry('780x520')
            label(window, 'Check the files below, then choose Copy to card.')
            box = tk.Text(window, wrap='none', height=18)
            scroll = ttk.Scrollbar(window, command=box.yview)
            box.configure(yscrollcommand=scroll.set)
            scroll.pack(side='right', fill='y')
            box.pack(fill='both', expand=True)
            box.insert('1.0', str(root) + '\n\n' + text)
            box.configure(state='disabled')
            self.app.bind_scroll_area(box)
            def dismiss():
                window.destroy()
                self.copying = False
                self.go.state(['!disabled'])
                self.set_status('sd', 'Copy cancelled. Nothing was copied.')
                self.app.update_nav()
            def approve():
                window.destroy()
                self._copy_approved(root, items, tool)
            button(window, text='Copy to card', command=approve, primary=True).pack(side='right', padx=8, pady=8)
            button(window, text='Cancel', command=dismiss).pack(side='right', padx=8, pady=8)
            window.protocol('WM_DELETE_WINDOW', dismiss)
            window.transient(self.app.root)
            window.grab_set()
        self.app.work(lambda: deliver.sd_preview(root, self.st.product, self.st.artifacts,
                      self.st.folder, self.app.opts.work, self.st.parts), prepared, errors=True)

    def _copy_approved(self, root, items, tool):
        self.set_status("sd", "Copying...")
        self.show_progress("sd", 0)
        self.app.update_nav()
        prog = lambda done, total, name: self.app.call(self.show_progress, "sd", 100.0 * done / max(1, total))  # noqa

        def after(res):
            self.copying = False
            self.go.state(["!disabled"])
            if isinstance(res, Exception):
                self.set_status("sd", str(res))
            else:
                copied, skipped, nbytes, tex = res
                self.show_progress("sd", 100)
                msg = "Copied %d files (%.1f MB) to %s (%d unchanged)." % (copied, nbytes / 1e6, root, skipped)
                msg += "\n" + tex.message
                if deliver.dsp_missing(root):
                    msg += "\n\n" + deliver.DSP_MESSAGE
                self.set_status("sd", msg)
                self.st.sd_root = root
                self.st.deliver_how = "sd"
                self.st.delivered = True
            self.app.update_nav()
        self.app.work(lambda: deliver.copy_with_textures(root, items, self.st.product, self.st.folder,
                                                         self.app.opts.work, self.st.parts, prog, tool=tool), after, errors=True)

    def send(self):
        if self.copying:
            return
        try:
            host, port = ftp_delivery.endpoint(self.addr.get())
            addr = '%s:%d' % (host, port)
        except OSError as exc:
            self.set_status('wifi', str(exc))
            return
        self.wifi_busy(True)
        self.st.delivered = False
        self.set_status('wifi', 'Preparing files and textures on the PC...')
        self.show_progress("wifi", 0)

        def job():
            return ftp_delivery.send_product(addr, self.st.product, self.st.artifacts, self.st.folder,
                    self.app.opts.work, self.st.parts, cancelled=self.cancelled.is_set,
                    log=lambda text: self.app.call(self.set_status, 'wifi', text), progress=self.ftp_progress)

        def after(res):
            self.wifi_busy(False)
            self.set_status("wifi", ftp_delivery.error_text(res) if isinstance(res, Exception) else "Sent and checked. You can close ftpd.")
            if not isinstance(res, Exception):
                self.st.sd_root = None
                self.st.ftp_console = addr
                self.st.files_root = res.root
                self.st.deliver_how = "wifi"
                self.st.delivered = True
                preferences.save(self.app.opts.work, self.app.boring.get(), console=addr)
            self.app.update_nav()
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
            return  # The completion callback supplies the final status.
        # Bound Tk's event queue during fast local/mock transfers.
        now = __import__('time').monotonic()
        if now - getattr(self, '_ftp_update', 0) < 0.15:
            return
        self._ftp_update = now
        left = _fmt_secs(event.seconds_left) if event.seconds_left is not None else 'measuring...'
        text = '%s: %s\n%.1f MB/s, about %s left' % (event.phase, event.path, event.speed/1e6, left)
        self.app.call(self.set_status, 'wifi', text)
        self.app.call(self.show_progress, 'wifi', 100*event.done/max(1, event.total))

    def find_console(self, permission_retry=False):
        if self.copying:
            return
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
                    if messagebox.askretrycancel('Find my 3DS',
                            'No 3DS answered. If Local Network access was blocked, allow this builder in System Settings > Privacy & Security > Local Network, then choose Retry.',
                            parent=self.app.root):
                        self.find_console(permission_retry=True)
            else:
                self.addr.config(values=found)
                self.addr.set(found[0])
                self.set_status('wifi', 'Found a 3DS. Check that the address matches the one ftpd shows, then click Send.')
        # Paint and finish the button-release event before discovery can trigger
        # macOS's Local Network prompt. Never update Tk from a probe thread.
        self.app.root.after_idle(lambda: self.app.work(job, after, errors=True))

    def can_next(self):
        return self.st.delivered and not self.copying

    def busy(self):
        return self.copying

    def auto(self):
        ap = self.app.opts.autopilot or {}
        if self.copying:
            return "wait"
        if self.st.delivered:
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
            if not getattr(self, "_auto_started", False):
                self._auto_started = True
                self.copy()
                return "wait"
            return ("fail", self.texts["sd"])
        return ("fail", "no SD path in the autopilot plan")


class SavesPage(Page):
    title = "Import PC saves"

    def __init__(self, app):
        super().__init__(app)
        self.intro = label(self.frame, "")
        self.folder_label = label(self.frame, "", bold=True)
        row = ttk.Frame(self.frame)
        row.pack(anchor="w", pady=(0, 8))
        button(row, text="Scan again", command=self.scan).pack(side="left", padx=(0, 8))
        button(row, text="Choose folder...", command=self.choose).pack(side="left")
        self.tree = ttk.Treeview(self.frame, columns=("file", "date", "summary", "status"), show="headings", height=6,
                                 selectmode="none")
        for c, w, heading in (("file", 90, "File"), ("date", 150, "Saved"),
                              ("summary", 180, "Name / LV"), ("status", 110, "Status")):
            self.tree.heading(c, text=heading)
            self.tree.column(c, width=w, minwidth=60, stretch=(c == "summary"))
        self.tree.pack(fill="x", pady=(0, 8))
        self.notice = label(self.frame, "")
        self.choice = tk.StringVar(value="skip")
        self.copy_radio = radiobutton(self.frame, text="Copy PC saves to SD card", value="copy",
                                      variable=self.choice, command=self.changed)
        self.copy_radio.pack(anchor="w")
        self.skip_radio = radiobutton(
            self.frame, text="Skip", value="skip",
            variable=self.choice, command=self.changed)
        self.skip_radio.pack(anchor="w", pady=(0, 8))
        self.status = label(self.frame, "")
        self.custom_dir = None
        self.ftp_snapshot = None
        self.ftp_running = False
        self.ftp_cancel = threading.Event()
        self.ftp_cancel_button = button(self.frame, text='Cancel', command=self.ftp_cancel.set)
        self.ftp_cancel_button.pack_forget()

    def on_show(self):
        self.intro.config(text="Copy your PC saves to the 3DS. Your PC files are not changed.")
        self.scan()

    def target_save_dir(self):
        if self.custom_dir:
            return self.custom_dir
        return self.app.opts.probes.pc_save_dir(self.st.game)

    def choose(self):
        d = filedialog.askdirectory(title="Select %s PC save folder" % self.st.product.title)
        if d:
            self.custom_dir = os.path.normpath(d)
            self.scan()

    def scan(self):
        p = self.st.product
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
        if not scan.exists or (not valid and not corrupt):
            notes.append("No PC saves were found in %s." % scan.folder)
        elif corrupt:
            notes.append("Can't read %s.\n"
                         "Choose another folder or select Skip." % ", ".join(s.name for s in corrupt))
        else:
            notes.append("Found %d PC save file%s." % (len(valid), "" if len(valid) == 1 else "s"))
        existing = deliver.existing_sd_save_files(self.st.sd_root, p) if self.st.sd_root else []
        existing_names = [os.path.basename(x) for x in existing]
        if existing:
            notes.append("SD card already has 3DS saves (%s); copying backs them up first."
                         % ", ".join(existing_names))
        wifi = self.st.deliver_how == 'wifi' and self.st.ftp_console
        if not self.st.sd_root and not wifi:
            notes.append("No SD card selected. Copy saves manually to sd:/%s/." % p.sd_save_dir())
        self.notice.config(text="\n".join(notes))
        self.status.config(text="")
        if scan.files and (self.st.sd_root or wifi):
            self.copy_radio.state(["!disabled"])
            self.choice.set("skip" if (existing or corrupt) else "copy")
        else:
            self.copy_radio.state(["disabled"])
            self.choice.set("skip")
        self.changed()
        if wifi:
            self.copy_radio.config(text='Copy PC saves to the 3DS over Wi-Fi')
            self.choice.set('skip')
            self.ftp_snapshot = None
            self.ftp_running = True
            self.ftp_cancel.clear()
            self.status.config(text='Checking the 3DS saves...')
            def after(result):
                self.ftp_running = False
                if isinstance(result, Exception):
                    self.status.config(text=ftp_saves.error_text(result, connecting=True))
                    self.copy_radio.state(['disabled'])
                else:
                    self.ftp_snapshot = result
                    self.status.config(text=('3DS saves found. Copying will back them up first.' if result.files else
                                             'No 3DS saves found.'))
                self.changed()
            self.app.work(lambda: ftp_saves.inspect(self.st.ftp_console, p, cancelled=self.ftp_cancel.is_set),
                          after, errors=True)

    def changed(self):
        self.app.update_nav()

    def on_next(self):
        self.status.config(text="")
        if self.choice.get() == 'copy' and self.st.deliver_how == 'wifi' and self.st.ftp_console:
            return self.import_ftp()
        if self.choice.get() != "copy" or not self.st.sd_root or not self.st.save_scan:
            self.st.save_result = None
            self.st.saves_done = True
            return True
        if not self.st.save_scan.files:
            self.st.save_result = None
            self.st.saves_done = True
            return True
        p = self.st.product
        existing = deliver.existing_sd_save_files(self.st.sd_root, p)
        if existing and not self.st.save_scan.has_corrupt:
            confirm = self.app.opts.confirm_overwrite or messagebox.askyesno
            sd_who = (deliver.scan_sd_saves(self.st.sd_root, p).player_summary
                      or ", ".join(os.path.basename(x) for x in existing))
            pc_who = self.st.save_scan.player_summary or "PC files"
            prompt = ("Replace the 3DS save (%s) with the PC save (%s)?\n"
                      "The 3DS saves are backed up first." % (sd_who, pc_who))
            if not confirm("Replace 3DS saves?", prompt):
                self.status.config(text="3DS saves kept.\nSelect Skip to go on.")
                return False
        try:
            res = deliver.copy_pc_saves(self.st.sd_root, p, self.st.save_scan, overwrite=bool(existing))
        except deliver.DeliverError as e:
            self.status.config(text=str(e))
            return False
        self.st.save_result = res
        self.st.saves_done = True
        return True

    def import_ftp(self):
        if self.ftp_running or self.ftp_snapshot is None:
            return False
        if self.st.save_scan.has_corrupt or not self.st.save_scan.valid_files:
            self.status.config(text='No readable PC saves to copy, so nothing was copied. Choose another folder or skip.')
            return False
        existing = self.ftp_snapshot
        if existing.files:
            confirm = self.app.opts.confirm_overwrite or messagebox.askyesno
            prompt = ('Replace the 3DS save (%s) with the PC save (%s)?\n'
                      'The 3DS saves are backed up first.' %
                      (existing.player_summary or ', '.join(existing.files),
                       self.st.save_scan.player_summary or 'PC files'))
            if not confirm('Replace 3DS saves?', prompt):
                self.status.config(text='3DS saves kept.\nSelect Skip to go on.')
                return False
        self.ftp_running = True
        self.ftp_cancel.clear()
        self.ftp_cancel_button.pack(anchor='w')
        self.status.config(text='Backing up the 3DS saves, then copying...')
        self.app.update_nav()
        def after(result):
            self.ftp_running = False
            self.ftp_cancel_button.pack_forget()
            if isinstance(result, Exception):
                self.status.config(text=ftp_saves.error_text(result))
            else:
                self.st.save_result = result
                self.st.saves_done = True
                self.app.show(self.app.index+1)
            self.app.update_nav()
        self.app.work(lambda: ftp_saves.copy(self.st.ftp_console, self.st.product, self.st.save_scan, existing,
                        overwrite=bool(existing.files), cancelled=self.ftp_cancel.is_set), after, errors=True)
        return False

    def busy(self):
        return self.ftp_running

    def auto(self):
        ap = self.app.opts.autopilot or {}
        if ap.get("save_dir") and self.custom_dir != os.path.normpath(ap["save_dir"]):
            self.custom_dir = os.path.normpath(ap["save_dir"])
            self.scan()
        if "import_saves" in ap:
            if ap["import_saves"]:
                if not (self.st.save_scan and self.st.save_scan.valid_files and self.st.sd_root):
                    return ("fail", "autopilot requested save import, but no valid PC saves or SD card were available")
                self.choice.set("copy")
            else:
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
        self.cleanup_box = ttk.Frame(self.frame)
        self.cleanup_box.pack(fill='x', pady=(12, 0))
        label(self.cleanup_box, 'Free up space on this PC (optional)', bold=True)
        self.cleanup_choices = {}
        self.cleanup_checks = {}
        for key, title in [('build', 'Build files and caches'), ('downloads', 'Downloaded installers'),
                           ('tools', '3DS compiler and tools this builder installed')]:
            value = tk.BooleanVar(value=False)
            widget = checkbutton(self.cleanup_box, text=title, variable=value)
            widget.pack(anchor='w')
            self.cleanup_choices[key] = value
            self.cleanup_checks[key] = (widget, title)
        label(self.cleanup_box, 'Removing build files makes the next build slower. Removed tools are installed again from the compiler page when needed.')
        self.cleanup_notice = label(self.cleanup_box, '')
        self.cleanup_preview = button(self.cleanup_box, text='Show files to delete', command=self.preview_cleanup)
        self.cleanup_preview.pack(anchor='w')

    def on_show(self):
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
        res = self.st.save_result
        if res and res.copied:
            # The count and folder are in the steps below (deliver.instructions).
            if res.backup_dir:
                summary.append("Previous 3DS saves backed up to: %s" % res.backup_dir)
        else:
            summary.append("PC saves: not copied.")
        if self.st.sd_root and deliver.dsp_missing(self.st.sd_root):
            summary.append(deliver.DSP_MESSAGE)
        self.summary_label.config(text="\n".join(summary))
        self.steps_label.config(text=deliver.instructions(p, self.st.targets, self.st.outputs, self.st.sd_root,
                                                          saves_result=res, delivery=self.st.deliver_how))
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

    def refresh_cleanup(self):
        self.cleaning = True
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
                self.cleanup_notice.config(text='\n'.join(result.kept) or 'Your game and finished builds are kept.')
                self.cleanup_preview.state(['!disabled'])
            self.app.update_nav()
        self.app.work(lambda: cleanup.plan(self.app.opts.work, protected=(self.st.folder, self.st.sd_root,
                                                                         self.st.files_root)), after, errors=True)

    def preview_cleanup(self):
        selected = [key for key, choice in self.cleanup_choices.items() if choice.get()]
        plan = getattr(self, 'cleanup_plan', None)
        if plan is None:
            return  # Keep the counting error, rather than asking for a checkbox.
        if not selected:
            self.cleanup_notice.config(text='Tick what to remove first.')
            return
        window = tk.Toplevel(self.frame)
        window.title('Show files to delete')
        window.geometry('760x480')
        label(window, 'Only the files listed here will be deleted. Your game and finished builds are kept.')
        box = ttk.Frame(window)
        box.pack(fill='both', expand=True)
        text = tk.Text(box, wrap='none')
        scroll = ttk.Scrollbar(box, command=text.yview)
        text.config(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        text.pack(fill='both', expand=True)
        text.insert('1.0', plan.preview(selected))
        text.config(state='disabled')
        self.app.bind_scroll_area(text)
        def remove():
            window.destroy()
            self.cleaning = True
            self.cleanup_preview.state(['disabled'])
            self.cleanup_notice.config(text='Removing the selected files...')
            self.app.update_nav()
            def after(result):
                self.cleaning = False
                if isinstance(result, Exception):
                    self.cleanup_notice.config(text=str(result))
                else:
                    deleted, kept = result
                    self.cleanup_notice.config(text='Removed %d files.\n%s' % (len(deleted), '\n'.join(kept)))
                self.app.update_nav()
            self.app.work(lambda: cleanup.remove(plan, selected), after, errors=True)
        button(window, text='Delete these files', command=remove).pack(side='right', padx=8, pady=8)
        button(window, text='Cancel', command=window.destroy).pack(side='right', padx=8, pady=8)
        window.transient(self.app.root)
        window.grab_set()

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
        root.title(("%s builder" % products.PRODUCTS[opts.game].app_name) if opts.game else "3DS builder")
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
        nav = ttk.Frame(root, padding=(16, 8, 16, 12))
        nav.pack(side="bottom", fill="x")
        self.body.pack(fill="both", expand=True)
        button(nav, text="About", command=self.about).pack(side="left")
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
        root.bind_all("<KeyRelease-ISO_Left_Tab>", self._focused, add="+")
        root.protocol("WM_DELETE_WINDOW", self.close)
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
        page = self.page
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

    def bind_scroll_area(self, widget):
        """Dialogs have no Page; give their text areas the same precise scrolling."""
        tag = 'BuilderScrollArea' + str(widget)
        def scroll(event):
            axis = getattr(event, '_scroll_axis', 'x' if getattr(event, 'state', 0) & 1 else 'y')
            if hasattr(event, '_scroll_amount'):
                amount = event._scroll_amount / 16
            else:
                event._tk9_wheel = int(str(widget.tk.call('package', 'require', 'Tk')).split('.')[0]) >= 9
                amount = wheel_units(event, sys.platform)
            key = '_wheel_remainder_' + axis
            remainder = getattr(widget, key, 0.0) + amount
            units = int(remainder)
            setattr(widget, key, remainder - units)
            if units:
                getattr(widget, axis + 'view_scroll')(units, 'units')
            return 'break'
        for seq in ('<MouseWheel>', '<Shift-MouseWheel>', '<Button-4>', '<Button-5>',
                    '<Shift-Button-4>', '<Shift-Button-5>'):
            widget.bind_class(tag, seq, scroll)
        bind_touchpad(widget, tag, scroll)
        widget.bindtags((tag,) + widget.bindtags())

    def _focused(self, event):
        widget = event.widget
        if isinstance(widget, tk.Misc) and str(widget).startswith(str(self.page.frame) + "."):
            self.root.after_idle(lambda: widget.winfo_exists() and self.page.show_widget(widget))

    def toggle_boring(self):
        self.theme.boring = self.boring.get()
        self.theme.apply()
        preferences.save(self.opts.work, self.theme.boring)

    @property
    def page(self):
        return self.pages[self.index]

    def reset(self, game):
        self.state = State(game)
        self.pages[1].searched_for = None

    def show(self, i):
        if self.state.folder:
            self.theme.load_from_game_dir(self.state.folder)
        self.page.box.pack_forget()
        self.index = i
        self.heading.config(text="* " + self.page.title)
        self.stepno.config(text="Step %d of %d" % (i + 1, len(self.pages)))
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

    def update_nav(self):
        last = self.index == len(self.pages) - 1
        self.next_btn.config(text="Finish" if last else "Next >")
        ok = self.page.can_next() and not self.page.busy()
        if last:
            ok = not self.page.busy()
        self.next_btn.state(["!disabled"] if ok else ["disabled"])
        self.back_btn.state(["!disabled"] if self.index > 0 and not self.page.busy() else ["disabled"])

    def next(self):
        if self.index == len(self.pages) - 1:
            if self.page.busy():
                return
            self.root.destroy()
            return
        if self.page.can_next() and not self.page.busy():
            if not self.page.on_next():
                self.update_nav()
                return
            self.show(self.index + 1)

    def back(self):
        if self.index > 0 and not self.page.busy():
            self.show(self.index - 1)

    def about(self):
        app = products.PRODUCTS[self.state.game].app_name
        messagebox.showinfo("About", ABOUT.format(app=app, source=SOURCE_URL.format(app=app)))

    def close(self):
        if self.page.busy() and self.index != 5:
            messagebox.showinfo('Please wait', 'Wait for this step to finish, or click Cancel, before closing.')
            return
        b = self.pages[5]
        if b.running:
            if not messagebox.askyesno("Quit", "Stop the build and quit?"):
                return
            if b.build:
                b.build.cancel()
        self.root.destroy()

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
        try:
            while True:
                fn, args = self.q.get_nowait()
                if len(args) == 1 and isinstance(args[0], dict) and getattr(fn, "__name__", "") in ("config",
                                                                                                     "configure"):
                    fn(**args[0])
                else:
                    fn(*args)
        except queue.Empty:
            pass
        if isinstance(self.page, BuildPage) and self.page.running and self.page.build is not None:
            self.page.progress(self.page.build.progress)
        self.page.fit()
        self.root.after(100, self.poll)

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


def run(opts: Options):
    root = tk.Tk()
    app = App(root, opts)
    root.mainloop()
    return app.exit_code
