"""Build UNDERTALE's sound plan for the RomFS (romfs:/roomplan/undertale-sfx.dtp; format and runtime:
platform/3ds/source/z_utsfx.c).

    py -3 -I tools/roomplan/build_sfxplan.py --out <file.dtp> [--data <game folder>] [--records <dir>]

Finds the game's data.win (--data, $CTRPORT_UNDERTALE_DATA, or the Steam library, as tools/aot/prepare.py does), picks
the committed sound record (tools/roomplan/records/sounds/*.json, made by sound_records.py) made on that exact file
(SHA-256), and writes per scene (a room; a battle room's battle group) the sounds it played that the 3DS copies into its
sound cache at their first play: embedded PCM WAVs read into memory (under 512 KiB in data.win; bigger ones stream from
their section) whose copy fits the cache entry cap. Each entry carries its copy's size (stereo WAVs whose channels are
identical are kept as mono, as ctr_audio_system.c's buildSfx does), and each scene keeps within SCENE_BYTES, in first-play
order; each scene also says how many of them lead its first FIRST_FRAMES frames (the 3DS copies those in the room
change's frame, the rest on a worker thread). The same inputs always give the same bytes.

Exit status: 0 written, 3 no game data or no record for it (the build goes on without a plan), 2 an error.
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'aot'))

MAGIC = 0x50535444            # "DTSP" (UTSFX_MAGIC)
VERSION = 2
NO_SUB = 0xFFFF
SECTION_MIN = 512 * 1024      # AUDO_SECTION_MIN_BYTES: from this size a sound streams from its section of data.win
ENTRY_MAX = 2 * 1024 * 1024   # SFX_CACHE_MAX_ENTRY_BYTES
SCENE_BYTES = 2 * 1024 * 1024  # copies planned per scene at most (the cache holds 6 MiB; Old linear memory is tight)
MAX_SIZE = 256 * 1024         # UTSFX_MAX_SIZE
FIRST_FRAMES = 10             # a sound first played before this frame of the visit is copied in the room change's frame
FLAG_EMBEDDED = 0x01
FLAG_COMPRESSED = 0x02
FLAG_REGULAR = 0x64


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def chunks(d):
    out = {}
    o = 8
    while o + 8 <= len(d):
        name = d[o:o + 4].decode('ascii', 'replace')
        size = struct.unpack_from('<I', d, o + 4)[0]
        out[name] = (o + 8, size)
        o += 8 + size
    return out


def wav_copy_bytes(w):
    """The bytes buildSfx copies for a WAV blob, or None if the 3DS doesn't copy it (not PCM 8/16-bit mono/stereo,
    empty, or over the cache entry cap)."""
    if len(w) < 12 or w[:4] != b'RIFF' or w[8:12] != b'WAVE':
        return None
    k = 12
    fmt = None
    while k + 8 <= len(w):
        cid = w[k:k + 4]
        length = struct.unpack_from('<I', w, k + 4)[0]
        if cid == b'fmt ' and length >= 16:
            tag, ch, rate = struct.unpack_from('<HHI', w, k + 8)
            bits = struct.unpack_from('<H', w, k + 22)[0]
            fmt = (tag, ch, rate, bits)
        elif cid == b'data':
            start = k + 8
            data_bytes = min(length, len(w) - start)
            break
        k += 8 + length + (length & 1)
    else:
        return None
    if fmt is None:
        return None
    tag, ch, rate, bits = fmt
    if tag != 1 or ch not in (1, 2) or bits not in (8, 16) or rate == 0:
        return None
    if data_bytes > ENTRY_MAX:
        return None
    bps = bits // 8
    frames = data_bytes // (ch * bps)
    if frames == 0:
        return None
    out_ch = ch
    if ch == 2:
        s = w[start:start + frames * 2 * bps]
        if bps == 2:
            dual = s[0::4] == s[2::4] and s[1::4] == s[3::4]
        else:
            dual = s[0::2] == s[1::2]
        if dual:
            out_ch = 1
    return frames * out_ch * bps


def read_sounds(path):
    """Per SOND index: {'name', 'copy'} where copy is the bytes the 3DS copies into its sound cache at the first play
    (None: not a sound the plan can take)."""
    with open(path, 'rb') as f:
        d = f.read()
    ch = chunks(d)

    def string(p):
        n = struct.unpack_from('<I', d, p - 4)[0]
        return d[p:p + n].decode('utf-8', 'replace')

    so, _ = ch['SOND']
    n = struct.unpack_from('<I', d, so)[0]
    ptrs = struct.unpack_from('<%dI' % n, d, so + 4)
    ao, alen = ch['AUDO']
    na = struct.unpack_from('<I', d, ao)[0]
    aptrs = struct.unpack_from('<%dI' % na, d, ao + 4)
    aend = ao + alen
    out = []
    for p in ptrs:
        name = string(struct.unpack_from('<I', d, p)[0])
        flags = struct.unpack_from('<I', d, p + 4)[0]
        group = struct.unpack_from('<i', d, p + 28)[0]
        af = struct.unpack_from('<i', d, p + 32)[0]
        regular = (flags & FLAG_REGULAR) == FLAG_REGULAR
        in_audo = not regular or (flags & (FLAG_EMBEDDED | FLAG_COMPRESSED)) != 0
        copy = None
        if in_audo and group == 0 and 0 <= af < na:
            q = aptrs[af]
            size = struct.unpack_from('<I', d, q)[0]
            # The lazy parser may take the distance to the next entry as the size (AudioEntry.sizeIsUpperBound).
            bound = (aptrs[af + 1] if af + 1 < na else aend) - q - 4
            if 0 < size and max(size, bound) < SECTION_MIN:
                copy = wav_copy_bytes(d[q + 4:q + 4 + size])
        out.append({'name': name, 'copy': copy})
    return out


def build(info, sounds, record):
    """The plan's bytes. info: dwinfo.DataWinInfo; sounds: read_sounds(); record: a sound record for this data.win."""
    scenes = []
    for s in record['scenes']:
        room, sub = s['room'], s['sub']
        if not 0 <= room < info.room_count:
            continue
        entries = []
        total = 0
        first = 0
        for df, k in s['sounds']:
            copy = sounds[k]['copy'] if 0 <= k < len(sounds) else None
            if copy is None or total + copy > SCENE_BYTES:
                continue
            total += copy
            entries.append((k, (copy + 1023) // 1024))
            if df < FIRST_FRAMES and first == len(entries) - 1:
                first += 1
        if entries:
            scenes.append((room, NO_SUB if sub < 0 else sub, entries, first))
    scenes.sort(key=lambda t: (t[0], t[1]))
    body = bytearray()
    for room, sub, entries, first in scenes:
        body += struct.pack('<HHHH', room, sub, len(entries), first)
        for k, kb in entries:
            body += struct.pack('<HH', k, kb)
    head = struct.pack('<II32sIIIIII', MAGIC, VERSION, bytes.fromhex(info.sha256), len(sounds), info.room_count,
                       len(scenes), len(body), fnv1a(body), 0)
    assert len(head) == 64
    data = head + bytes(body)
    if len(data) > MAX_SIZE:
        raise ValueError('sound plan of %d bytes is over %d' % (len(data), MAX_SIZE))
    return data


def find_record(records_dir, sha):
    if not os.path.isdir(records_dir):
        return None
    for name in sorted(os.listdir(records_dir)):
        if name.endswith('.json'):
            with open(os.path.join(records_dir, name), 'r', encoding='ascii') as f:
                record = json.load(f)
            if record.get('kind') == 'sounds' and record.get('data_sha256') == sha:
                return record
    return None


def write_plan(path, records, out):
    """0 written, 3 no record for this data.win, 2 a record that doesn't fit."""
    import dwinfo
    info = dwinfo.DataWinInfo(path)
    record = find_record(records, info.sha256)
    if record is None:
        print('Sound plan: no record for %s (SHA-256 %s)' % (path, info.sha256))
        return 3
    sounds = read_sounds(path)
    if record.get('format') != 1 or record.get('room_count') != info.room_count or record.get('sound_count') != len(sounds):
        print('Sound plan: the record for SHA-256 %s does not fit this data.win' % info.sha256, file=sys.stderr)
        return 2
    data = build(info, sounds, record)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, out)
    count = struct.unpack_from('<I', data, 48)[0]
    print('Sound plan: %s, %d scenes, %d bytes, record %s %s %s' % (out, count, len(data), record['game'], record['store'],
                                                                  record['version']))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', help='the game folder (default: as tools/aot/prepare.py finds it)')
    ap.add_argument('--data-win', help='the data.win itself (tests)')
    ap.add_argument('--records', default=os.path.join(HERE, 'records', 'sounds'))
    ap.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    if a.data_win is not None:
        path = a.data_win
    else:
        import prepare  # tools/aot/prepare.py: the same search as the AOT step
        import corpus
        folder = prepare.find_data('undertale', a.data)
        found = corpus.parts('undertale', folder) if folder else []
        path = found[0][1] if found else None
    if path is None or not os.path.isfile(path):
        print('Sound plan: no undertale data.win found')
        return 3
    try:
        return write_plan(path, a.records, a.out)
    except (ValueError, KeyError, struct.error) as e:
        print('Sound plan: %s' % e, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
