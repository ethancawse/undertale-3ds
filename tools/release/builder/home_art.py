"""Generate approved HOME Menu art in the private build mirror, from the player's own files.

UT icons prefer the verified executable's original SOUL. Steam art is the next source, then FORM assets.
Only names, source hashes, measured grids and recovered hashes live here; no game pixels or sounds.
"""
from __future__ import annotations

import bz2
import dataclasses
import hashlib
import io
import json
import math
import mmap
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import wave
import zlib
from processes import hidden_options

from art_pixels import Image, blank, centred, nearest, paste, png, read_png, read_qoi, trim


class ArtError(ValueError):
    pass


@dataclasses.dataclass(frozen=True)
class CacheArt:
    names: tuple
    size: tuple
    sha256: str
    grid: tuple                 # source cell width/height, x/y phase (measured at review)
    recovered_size: tuple
    recovered_sha256: str       # SHA256(LE width, LE height, canonical RGBA)
    kind: str = "logo"


# Source grids were detected by paired edge-period/phase fitting. Hashes bind each measurement to those bytes.
# DR's recovered header wordmark agrees with logo.png and across five detection thresholds, byte for byte.
CACHE_ART = {
    "undertale": {
        "banner": CacheArt(("header.jpg", "library_header.jpg"), (460, 215),
            "924469d6bce07a5faa3ab8461b320def4029fd320ecad9400854c75e3f36a872", (4, 4, 2, 0), (90, 9),
            "9ad05ee0aff98f15ee8e61d83ace9ed1c4d641f1bfbe5371d591cf24d3bcac63"),
        "icon": CacheArt(("2ce672b89b63ec1e70d2f12862e72eb4a33e9268.jpg", "icon.jpg"), (32, 32),
            "ea481dc7c1f1e86b7fd7d4177d8c1c749d050502fb70369c0c021e1bb3760365", (2, 2, 0, 0), (16, 16),
            "6cb603701ccd4aab8ebb3f288a2a130e387676aa38a39926acb6d4409b811d78", "red"),
    },
    "deltarune": {
        "banner": CacheArt(("header.jpg", "library_header.jpg"), (460, 215),
            "c05dd1ec7ef3fcd648464f676577658aa828fa892958f5c5d22031fc4637fdb5", (3.244, 3.312, .0086, .0956), (112, 17),
            "82e279ffc5ed83592a0efe486cd674ee42fdc9033ac1b2be8f5afe6518edc6b5"),
        "icon": CacheArt(("f76969de63cf2f4eb11bc4a1c17e67beb590a9c5.jpg", "icon.jpg"), (32, 32),
            "9e86a35a34ff001f2526f4bb39825d138418725bea8ebe05378e2d3514b1984a", (1, 1, 0, 0), (32, 32),
            "29de2d49b20bfbd3bc4a218d4e5dcc5b967a9e1d684602919347f8879cbef065", "white"),
    },
}
APPIDS = {"undertale": "391540", "deltarune": "1671210"}
PALETTE = (b"\0\0\0\0", b"\xff\xff\xff\xff", b"\xff\0\0\xff")
# Measured read-only from the owner's Steam and GOG Windows copies: identical executable and RT_ICON.
UT_EXE_SIZE = 4136960
UT_EXE_SHA256 = "c8c4191026bf5587a6fad120855b8b82ffb4fa0c3eaf10515be472ad84248e58"
UT_ICON_SHA256 = "17627228a14a62c099da418e399f581400fefd956dacc2fde2587a0dd299f818"


def pe_icon(data):
    """Read RT_ICON / 1 / English from PE32; no Windows APIs, code loading or execution.

    Format: learn.microsoft.com/en-us/windows/win32/debug/pe-format and
    learn.microsoft.com/en-us/windows/win32/menurc/resource-file-formats.
    Only the measured 16px, 4-bit, uncompressed DIB is supported. Other formats fall back.
    """
    def take(start, count):
        if start < 0 or count < 0 or start + count > len(data):
            raise ArtError("Truncated executable art")
        return data[start:start + count]

    def unpack(fmt, start):
        return struct.unpack(fmt, take(start, struct.calcsize(fmt)))

    pe, = unpack('<I', 60)
    if take(0, 2) != b'MZ' or take(pe, 4) != b'PE\0\0':
        raise ArtError("Unsupported executable art")
    count, = unpack('<H', pe + 6)
    optional_size, = unpack('<H', pe + 20)
    optional = pe + 24
    if not 1 <= count <= 96 or optional_size < 120 or unpack('<H', optional)[0] != 0x10b:
        raise ArtError("Unsupported executable art")
    resource, resource_size = unpack('<II', optional + 112)

    def offset(address, size):
        for index in range(count):
            section = optional + optional_size + index * 40
            rva, raw_size, raw = unpack('<III', section + 12)
            if rva <= address and address + size <= rva + raw_size:
                start = raw + address - rva
                take(start, size)
                return start
        raise ArtError("Unmapped executable art")

    base = offset(resource, resource_size)
    def resource_bytes(relative, size):
        if relative < 0 or relative + size > resource_size:
            raise ArtError("Invalid executable art directory")
        return take(base + relative, size)

    directory = 0
    for depth, wanted in enumerate((3, 1, 1033)):
        header = resource_bytes(directory, 16)
        entries = sum(struct.unpack_from('<HH', header, 12))
        if entries > 4096:
            raise ArtError("Invalid executable art directory")
        table = resource_bytes(directory + 16, entries * 8)
        matches = [value for name, value in struct.iter_unpack('<II', table) if name == wanted]
        if len(matches) != 1 or bool(matches[0] & 0x80000000) != (depth < 2):
            raise ArtError("Missing executable icon")
        directory = matches[0] & 0x7fffffff
    address, length, _, _ = struct.unpack('<IIII', resource_bytes(directory, 16))
    if length != 296:
        raise ArtError("Unsupported executable icon")
    dib = take(offset(address, length), length)
    if struct.unpack_from('<IiiHHI', dib) != (40, 16, 32, 1, 4, 0):
        raise ArtError("Unsupported executable icon")
    if struct.unpack_from('<I', dib, 32)[0] != 16:
        raise ArtError("Unsupported executable icon palette")
    pixels = bytearray()
    for y in range(16):
        for x in range(16):
            packed = dib[104 + (15 - y) * 8 + x // 2]
            colour = packed >> 4 if x % 2 == 0 else packed & 15
            b, g, r, _ = dib[40 + colour * 4:44 + colour * 4]
            transparent = dib[232 + (15 - y) * 4 + x // 8] & (128 >> (x % 8))
            pixels.extend((0, 0, 0, 0) if transparent else (r, g, b, 255))
    return Image(16, 16, bytes(pixels)), hashlib.sha256(dib).hexdigest()


def executable_icon(data_dir):
    """Read only the known executable in the already-verified game folder; never accept replacement art."""
    try:
        with (Path(data_dir) / 'UNDERTALE.exe').open('rb') as source:
            data = source.read(UT_EXE_SIZE + 1)
        if len(data) != UT_EXE_SIZE or hashlib.sha256(data).hexdigest() != UT_EXE_SHA256:
            return None, {}
        native, digest = pe_icon(data)
        if digest != UT_ICON_SHA256 or native.digest() != CACHE_ART['undertale']['icon'].recovered_sha256:
            return None, {}
        return native, dict(source="player executable", source_sha256=UT_EXE_SHA256,
                            resource_sha256=digest, native_rgba_sha256=native.digest())
    except (OSError, ArtError, struct.error):
        return None, {}


def recover(image, spec):
    """Vote a flat palette colour from the central half of every source cell. Never average edge colours."""
    fx, fy, px, py = spec.grid
    width, height = math.floor((image.width - px) / fx + 1e-6), math.floor((image.height - py) / fy + 1e-6)
    out = bytearray()
    for y in range(height):
        for x in range(width):
            x0 = min(round(px + (x + .25) * fx), image.width - 1)
            y0 = min(round(py + (y + .25) * fy), image.height - 1)
            x1, y1 = max(x0 + 1, round(px + (x + .75) * fx)), max(y0 + 1, round(py + (y + .75) * fy))
            values = []
            for sy in range(y0, min(y1, image.height)):
                for sx in range(x0, min(x1, image.width)):
                    r, g, b, a = image.pixel(sx, sy)
                    red, white = r >= 128 and g < 100 and b < 100, min(r, g, b) >= 128
                    value = (2 if red else 1 if white else 0) if spec.kind == "logo" else (
                        2 if red and spec.kind == "red" else 1 if white and spec.kind == "white" else 0)
                    values.append(value if a >= 128 else 0)
            counts = [values.count(i) for i in range(3)]
            winners = [i for i, count in enumerate(counts) if count == max(counts)]
            centre = values[len(values) // 2]
            out.extend(PALETTE[centre if centre in winners else winners[0]])
    result = Image(width, height, bytes(out))
    result = trim(result) if spec.kind == "logo" else result
    if (result.width, result.height) != spec.recovered_size or result.digest() != spec.recovered_sha256:
        raise ArtError("Steam art recovery did not match")
    return result


def jpeg_size(data):
    if data[:2] != b"\xff\xd8":
        raise ArtError("Not a JPEG")
    pos = 2
    while pos + 4 <= len(data):
        if data[pos] != 255:
            break
        while pos < len(data) and data[pos] == 255:
            pos += 1
        marker = data[pos]; pos += 1
        if marker in (0xd8, 0xd9):
            continue
        size = struct.unpack_from(">H", data, pos)[0]
        if size < 2 or pos + size > len(data):
            break
        if marker in (0xc0, 0xc1, 0xc2):
            height, width = struct.unpack_from(">HH", data, pos + 3)
            return width, height
        pos += size
    raise ArtError("Invalid JPEG dimensions")


def decode_cache(data, spec, tex3ds, work):
    """tex3ds pads the canvas to powers of two; its RGBA8 preview preserves original pixels at (0,0).

    ImageMagick writes raw preview channels at its quantum depth (8 or 16 bits, big endian). No mipmap/filter.
    Recovered hashes guard against differences between host JPEG decoders.
    """
    size = jpeg_size(data)
    if size != spec.size:
        raise ArtError("Unexpected Steam art size")
    with tempfile.TemporaryDirectory(prefix="art-decode-", dir=work) as tmp:
        source, preview = Path(tmp) / "source.jpg", Path(tmp) / "pixels.rgba"
        source.write_bytes(data)
        result = subprocess.run([str(tex3ds), "-f", "rgba8", "-p", str(preview), str(source)],
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=45, check=False, **hidden_options())
        if result.returncode or not preview.is_file():
            raise ArtError("Could not read Steam art")
        width, height = (max(8, 1 << (s - 1).bit_length()) for s in size)
        raw = preview.read_bytes()
        if len(raw) == width * height * 8:
            raw = raw[::2]       # RGBA8 preview, promoted to 16-bit quantum channels
        if len(raw) != width * height * 4:
            raise ArtError("Unsupported Steam art preview")
        return Image(width, height, raw).crop(0, 0, *size)


def cache_candidates(root, appid, names):
    cache = Path(root) / "appcache/librarycache"
    for name in names:
        yield cache / (appid + "_" + name)
        yield cache / appid / name
    # Steam's newer layout adds a content-hash directory. Never recurse through user libraries or userdata.
    try:
        children = sorted((cache / appid).iterdir())
    except OSError:
        return
    for child in children[:256]:
        if child.is_dir() and re.fullmatch(r"[0-9a-fA-F]{40}", child.name):
            for name in names:
                yield child / name


def steam_art(game, kind, roots, tex3ds, work, specs=CACHE_ART, decoder=decode_cache):
    spec = specs[game][kind]
    for root in roots:
        for path in cache_candidates(root, APPIDS[game], spec.names):
            try:
                if not path.is_file() or not 0 < path.stat().st_size <= 2 * 1024 * 1024:
                    continue
                data = path.read_bytes()
                if hashlib.sha256(data).hexdigest() != spec.sha256:
                    continue
                image = decoder(data, spec, tex3ds, work)
                if (image.width, image.height) != spec.size:
                    continue
                native = recover(image, spec)
                return native, {"source": "Steam", "source_sha256": spec.sha256,
                                "native_rgba_sha256": native.digest(), "grid": list(spec.grid)}
            except (ArtError, ValueError, IndexError, struct.error, subprocess.SubprocessError):
                continue
            except OSError as exc:
                # Read failures can fall back; write failures in our work folder must be reported as disk failures.
                if exc.errno in (28, 30, 122) or isinstance(exc, PermissionError) and exc.filename and (
                        Path(work).resolve() in Path(exc.filename).resolve().parents):
                    raise
    return None, {"source": "player data"}


class RuntimeArt:
    """Bounded, read-only FORM reader. Texture pages are decoded lazily; never reads save files."""
    def __init__(self, path):
        self.path = Path(path)
        try:
            self.file = self.path.open("rb")
        except OSError as exc:
            raise ArtError("Could not read the game's HOME Menu assets.") from exc
        try:
            self.data = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
            self.chunks, self.pages = {}, {}
            if self.data[:4] != b"FORM" or self.u32(4) + 8 != len(self.data):
                raise ArtError("Invalid game art container")
            pos = 8
            while pos < len(self.data):
                tag, size = struct.unpack_from("<4sI", self.data, pos)
                if pos + 8 + size > len(self.data):
                    raise ArtError("Truncated game art chunk")
                self.chunks[tag] = (pos + 8, size)
                pos += 8 + size
        except Exception as exc:
            self.close()
            if isinstance(exc, OSError):
                raise ArtError("Could not read the game's HOME Menu assets.") from exc
            raise

    def close(self):
        if hasattr(self, "data"):
            self.data.close()
        self.file.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def u32(self, offset):
        return struct.unpack_from("<I", self.data, offset)[0]

    def pointers(self, name):
        start, size = self.chunks[name]
        count = self.u32(start)
        if count > 100000 or 4 + count * 4 > size:
            raise ArtError("Invalid art pointer table")
        return struct.unpack_from("<%dI" % count, self.data, start + 4)

    def named(self, chunk, name):
        for ptr in self.pointers(chunk):
            address = self.u32(ptr)
            if self.data[address:address + len(name) + 1] == name.encode("ascii") + b"\0":
                return ptr
        raise ArtError("Missing player asset: " + name)

    def page(self, index):
        if index not in self.pages:
            entries = self.pointers(b"TXTR")
            start, size = self.chunks[b"TXTR"]
            if not hasattr(self, "texture_addresses"):
                self.texture_addresses = []
                for entry in entries:
                    address = None
                    for offset in range(entry, min(entry + 40, start + size) - 3, 4):
                        candidate = self.u32(offset)
                        if start <= candidate < start + size and self.data[candidate:candidate + 4] in (b"2zoq", b"fioq", b"\x89PNG"):
                            address = candidate
                            break
                    self.texture_addresses.append(address)
            address = self.texture_addresses[index]
            if address is None:
                raise ArtError("Missing player texture")
            # Bound at the next texture blob, not integers found inside its image header.
            following = [p for p in self.texture_addresses if p is not None and p > address]
            end = min(following + [address + 64 * 1024 * 1024, start + size])
            raw = self.data[address:end]
            if raw[:4] == b"2zoq":
                bz = raw.find(b"BZh", 8, 32)
                if bz < 0:
                    raise ArtError("Invalid compressed texture")
                raw = bz2.BZ2Decompressor().decompress(raw[bz:], max_length=80 * 1024 * 1024)
            self.pages[index] = read_png(raw) if raw[:4] == b"\x89PNG" else read_qoi(raw)
        return self.pages[index]

    def crop(self, pointer):
        sx, sy, sw, sh, tx, ty, tw, th, bw, bh, index = struct.unpack_from("<10Hh", self.data, pointer)
        if index < 0 or (sw, sh) != (tw, th):
            raise ArtError("Unsupported player art texture")
        return self.page(index).crop(sx, sy, sw, sh), (tx, ty, bw, bh)

    def sprite(self, name, frame=0):
        ptr = self.named(b"SPRT", name)
        width, height = struct.unpack_from("<II", self.data, ptr + 4)
        count_at = ptr + 56
        if self.u32(count_at) == 0xffffffff:
            version, kind = struct.unpack_from("<II", self.data, ptr + 60)
            if kind != 0:
                raise ArtError("Unsupported player sprite")
            count_at = ptr + 76 + 4 * (version >= 2) + 4 * (version >= 3)
        if not 0 <= frame < self.u32(count_at) < 10000:
            raise ArtError("Missing player sprite frame")
        image, (tx, ty, bw, bh) = self.crop(self.u32(count_at + 4 + 4 * frame))
        # Copy onto a transparent logical canvas without blending away the source alpha.
        if not (0 < width <= 256 and 0 < height <= 256 and tx + image.width <= width and ty + image.height <= height):
            raise ArtError("Unexpected player sprite size")
        out = bytearray(width * height * 4)
        for y in range(image.height):
            p = ((ty + y) * width + tx) * 4
            out[p:p + image.width * 4] = image.rgba[y * image.width * 4:(y + 1) * image.width * 4]
        return Image(width, height, bytes(out))

    def soul(self):
        image = self.sprite("spr_heartsmall")
        return Image(image.width, image.height, b"".join(bytes((255, 0, 0, image.rgba[p + 3]))
                                                         for p in range(0, len(image.rgba), 4)))

    def title(self, game):
        ptr = self.named(b"FONT", "fnt_maintext" if game == "undertale" else "fnt_main")
        atlas, _ = self.crop(self.u32(ptr + 28))
        for trial in range(5):
            pos = ptr + 40 + 4 * trial
            count = self.u32(pos)
            if 0 < count < 65536 and self.u32(pos + 4) == pos + 4 + count * 4:
                break
        else:
            raise ArtError("Missing player font glyphs")
        glyphs = {}
        for gp in struct.unpack_from("<%dI" % count, self.data, pos + 4):
            ch, x, y, w, h, shift, offset = struct.unpack_from("<5H2h", self.data, gp)
            if chr(ch) in game.upper():
                glyphs[chr(ch)] = (atlas.crop(x, y, w, h), shift, offset)
        width = sum(glyphs[c][1] for c in game.upper()) + 8
        height = max(glyphs[c][0].height for c in game.upper())
        if not (0 < width <= 256 and 0 < height <= 128):
            raise ArtError("Unexpected player font size")
        mask = bytearray(width * height * 4)
        x = 0
        for c in game.upper():
            glyph, shift, offset = glyphs[c]
            if x + offset < 0 or x + offset + glyph.width > width:
                raise ArtError("Invalid player font placement")
            for y in range(glyph.height):
                for gx in range(glyph.width):
                    p = (y * width + x + offset + gx) * 4
                    mask[p:p + 4] = bytes((255, 255, 255, glyph.pixel(gx, y)[3]))
            x += shift
        return trim(Image(width, height, bytes(mask)))

    def sound(self):
        ptr = self.named(b"SOND", "snd_select")
        group, index = struct.unpack_from("<2i", self.data, ptr + 28)
        if group < 0 or index < 0:
            raise ArtError("Missing snd_select audio")
        if group:
            with RuntimeArt(self.path.with_name("audiogroup%d.dat" % group)) as audio:
                return audio.audio(index)
        return self.audio(index)

    def audio(self, index):
        ptr = self.pointers(b"AUDO")[index]
        start, chunk_size = self.chunks[b"AUDO"]
        if not start <= ptr < start + chunk_size - 4:
            raise ArtError("Invalid snd_select audio pointer")
        size = self.u32(ptr)
        if not 0 < size <= 8 * 1024 * 1024 or ptr + 4 + size > start + chunk_size:
            raise ArtError("Invalid snd_select audio")
        data = self.data[ptr + 4:ptr + 4 + size]
        with wave.open(io.BytesIO(data)) as wav:
            if wav.getcomptype() != "NONE" or wav.getsampwidth() != 2 or wav.getnchannels() not in (1, 2):
                raise ArtError("Unsupported snd_select audio")
        return data


def fallback_banner(runtime, game):
    canvas = bytearray(blank(256, 128).rgba)
    border = (255, 255, 255, 255) if game == "undertale" else (157, 162, 196, 255)
    for y in range(5, 123):
        for x in range(5, 251):
            if x < 7 or x >= 249 or y < 7 or y >= 121:
                p = (y * 256 + x) * 4
                canvas[p:p + 4] = bytes(border)
    result = Image(256, 128, bytes(canvas))
    soul = nearest(runtime.soul(), 4)
    title = nearest(runtime.title(game), 2)
    return paste(paste(result, soul, (256 - soul.width) // 2, 18), title, (256 - title.width) // 2, 65)


def deltarune_icon(runtime):
    try:
        runtime.named(b'SPRT', 'spr_dkris_glowheart')
    except ArtError:
        # Later chapters have a red collision silhouette named "heart", not the actor holding it.
        # Use their actual Kris pose and the player's SOUL, at 1x, with the SOUL resting above the hands.
        pose = trim(runtime.sprite('spr_kris_pose', 0))
        soul = trim(runtime.soul())
        if pose.width > 30 or pose.height > 40 or soul.width > 12 or soul.height > 12:
            raise ArtError('Unexpected player icon size')
        x, y = (48 - pose.width - soul.width // 2) // 2, (48 - pose.height) // 2
        rendered = paste(blank(48, 48), pose, x, y)
        rendered = paste(rendered, soul, x + pose.width - soul.width // 2,
                         max(0, y + pose.height // 3 - soul.height))
        return rendered, dict(sprite='spr_kris_pose', soul_sprite='spr_heartsmall', frame=0, integer_scale=1,
                              native_rgba_sha256=pose.digest(), soul_rgba_sha256=soul.digest())
    native = runtime.sprite('spr_dkris_glowheart', 1)
    return centred(native, 48, 48, scale=1), dict(sprite='spr_dkris_glowheart', frame=1, integer_scale=1,
                                                native_rgba_sha256=native.digest())


def generate(game, data_dir, destination, tex3ds, roots=None, specs=CACHE_ART, decoder=decode_cache,
             runtime_factory=RuntimeArt, store="steam"):
    """Called after source mirroring, before CMake. Returns names/hashes only, suitable for a support log."""
    if store != "steam":
        roots = []
    elif roots is None:
        from products import verify_game
        roots = verify_game.default_steam_roots()
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    data_dir = Path(data_dir)
    data = data_dir / 'data.win'
    if game == 'deltarune':
        data = next((data_dir / ('chapter%d_windows/data.win' % i) for i in range(1, 6)
                     if (data_dir / ('chapter%d_windows/data.win' % i)).is_file()), data)
    result = {"schema": 1, "game": game}
    try:
        with runtime_factory(data) as runtime:
            for kind, size in (("banner", (256, 128)), ("icon", (48, 48))):
                native, info = executable_icon(data_dir) if game == 'undertale' and kind == 'icon' else (None, {})
                if native is None:
                    native, info = steam_art(game, kind, roots, tex3ds, destination, specs, decoder)
                if native is not None:
                    # Leave eight black pixels around Steam's 16x16 UT SOUL. Both SMDH packers
                    # halve this even grid exactly: the 24px icon has a native 16px SOUL and 4px padding.
                    scale = 2 if game == 'undertale' and kind == 'icon' else min(
                        size[0] // native.width, size[1] // native.height)
                    rendered = centred(native, *size, scale=scale)
                    info.update(integer_scale=scale)
                elif kind == "banner":
                    rendered = fallback_banner(runtime, game)
                    info.update(sprite="spr_heartsmall", font="fnt_maintext" if game == "undertale" else "fnt_main")
                else:
                    if game == 'deltarune':
                        rendered, details = deltarune_icon(runtime)
                        info.update(details)
                    else:
                        native = runtime.soul()
                        rendered = centred(native, *size, scale=4)
                        info.update(sprite='spr_heartsmall', frame=0, integer_scale=4,
                                    native_rgba_sha256=native.digest())
                encoded = png(rendered)
                # The mirror has one art file per target; both models intentionally share this approved artwork.
                for target in ("n3ds", "o3ds"):
                    (destination / ("%s-%s-%s.png" % (kind, game, target))).write_bytes(encoded)
                colours = {rendered.rgba[p:p + 3] for p in range(0, len(rendered.rgba), 4)}
                info.update(sha256=hashlib.sha256(encoded).hexdigest(), rgba_sha256=rendered.digest(),
                            size=list(size), colours=len(colours), sampling="nearest")
                result[kind] = info
            sound = runtime.sound()
            (destination / "banner.wav").write_bytes(sound)
            result["sound"] = {"source": "player data", "name": "snd_select", "sha256": hashlib.sha256(sound).hexdigest()}
    except (ValueError, KeyError, IndexError, struct.error, zlib.error, EOFError, wave.Error) as exc:
        raise ArtError("Could not read HOME Menu art from your game. Verify the game files and try again.") from exc
    (destination / "home-art.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result
