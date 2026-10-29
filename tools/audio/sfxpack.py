"""Build adpcm-sfx.dtsp: UNDERTALE's exact-dual-mono sound effects as one DSP-ADPCM pack (builder step).

Input: the player's data.win. Output: one file, copied to the SD card beside that data.win
(sdmc:/3ds/undertale-3ds/adpcm-sfx.dtsp). The runtime (platform/3ds/source/ctr_adpcm_pack.inc, build flag adpcmpack)
opens it once at audio startup and checks it by content: CATALOG_ID below, the data.win SHA-256 written here against
the boot data check's digest, and the exact file size. No receipt or timestamp, so copying it by FTP works. A missing
or mismatched pack plays every sound from its original PCM. Each row also carries a check of its own first 24 B and its
payload (row_check), which the runtime tests on the bytes it reads for each install: a damaged payload, or a row whose
sound, offset or length changed, plays that sound from its original PCM.

Eligible sounds: embedded RIFF WAVs in data.win (audio group 0) whose two channels are sample-for-sample identical
(mono_sfx.project). Each is encoded once to a one-channel DTAD v1 payload (dspadpcm.c through mono_sfx.encode);
--work keeps the encodes so a second run reuses them.

Layout (little-endian):
  header, 128 B: "DTSP", version 2, row count, pack bytes, catalog id (32 B), data.win SHA-256 as 64 lowercase hex
                 characters, 16 B zero.
  rows, 28 B each, sorted by sound index: u16 sound, u8 source bits (8/16), u8 0, u32 payload offset,
                 u32 payload bytes, u32 frames, u32 rate, u32 largest adjacent-sample step of the render (seam test),
                 u32 check (row_check: the row's first 24 B, then the payload).
  payloads: DTAD v1 files, each 4-byte aligned and whole 32-bit words.

Release check: tools/release/sfxpack-versions.json (names only) holds the expected pack size and SHA-256 for each
verified UNDERTALE data file. For a listed data.win the build fails (exit 1, output removed) unless its pack matches.

Usage: py -3 -I tools/audio/sfxpack.py --data-win <data.win> --out <adpcm-sfx.dtsp> [--work <dir>]
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dspadpcm as dsp
import mono_sfx as mono
import prepare

MAGIC = b'DTSP'
VERSION = 2
HEADER_BYTES = 128
ROW = struct.Struct('<HBBIIIIII')
ROW_HEAD = struct.Struct('<HBBIIIII')  # a row before its check: what row_check binds to the payload
CHECK_POLICY = 'fnv1a-words-row24-payload'  # row_check's definition; part of the catalog id
CATALOG_ID = hashlib.sha256(b'DTSP v2\0' + mono.POLICY.encode('ascii') + b'\0DTAD v1\0' +
                            CHECK_POLICY.encode('ascii')).digest()
NAME = 'adpcm-sfx.dtsp'
VERSIONS = Path(__file__).resolve().parents[2] / 'tools/release/sfxpack-versions.json'


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _encoded(blob: bytes, work: Path, lib_box: list, library=None):
    """The sound's DTAD payload and encode row, reusing a matching earlier encode in work/objects."""
    stem = work / 'objects' / mono.object_key(blob)
    payload, meta = stem.with_suffix('.dtad'), stem.with_suffix('.json')
    if payload.is_file() and meta.is_file():
        try:
            row = json.loads(meta.read_text(encoding='utf-8'))
            data = payload.read_bytes()
            if (row.get('projection') == mono.POLICY and row.get('source_sha256') == _sha256(blob)
                    and row.get('sha256') == _sha256(data)):
                return data, row
        except (OSError, ValueError):
            pass
    if not lib_box:
        lib_box.append(dsp.native(work / 'host', library))
    row = mono.encode(blob, stem, lib=lib_box[0])
    return payload.read_bytes(), row


def build(data_win, out, work=None, *, log=print, library=None):
    data_win, out = Path(data_win), Path(out)
    work = Path(work) if work else out.parent / (out.name + '.work')
    (work / 'objects').mkdir(parents=True, exist_ok=True)
    digest = _sha256(data_win.read_bytes())
    lib_box = []
    entries = []
    for sound in prepare.catalog(data_win):
        if sound.get('status') == 'absent' or not sound['embedded'] or sound['audio_group'] != 0:
            continue
        blob = prepare.embedded(data_win, sound)
        if blob[:4] != b'RIFF':
            continue
        try:
            projection = mono.project(blob)
        except ValueError as error:
            log('SKIP', sound['sound_index'], sound['sound_name'], error)
            continue
        if projection is None:
            continue
        payload, row = _encoded(blob, work, lib_box, library)
        assert row['channels'] == 1 and row['frames'] == projection.frames and row['rate_hz'] == projection.rate_hz
        if sound['sound_index'] > 0xFFFF:
            raise ValueError('Sound index exceeds the pack row')
        entries.append(dict(sound=sound['sound_index'], name=sound['sound_name'], bits=projection.source_bits,
                            payload=payload, frames=row['frames'], rate=row['rate_hz'],
                            seam=row['seam_max_step'], pcm_bytes=projection.frames * projection.source_bits // 8))
    if not entries:
        raise ValueError('No exact dual-mono sound effects in this data.win')
    entries.sort(key=lambda e: e['sound'])
    offset = HEADER_BYTES + ROW.size * len(entries)
    rows = bytearray()
    for e in entries:
        offset = (offset + 3) & ~3
        e['offset'] = offset
        head = ROW_HEAD.pack(e['sound'], e['bits'], 0, offset, len(e['payload']), e['frames'], e['rate'], e['seam'])
        rows += head + struct.pack('<I', row_check(head, e['payload']))
        offset += len(e['payload'])
    total = offset
    header = MAGIC + struct.pack('<III', VERSION, len(entries), total) + CATALOG_ID + digest.encode('ascii')
    header += bytes(HEADER_BYTES - len(header))
    image = bytearray(total)
    image[:HEADER_BYTES] = header
    image[HEADER_BYTES:HEADER_BYTES + len(rows)] = rows
    for e in entries:
        image[e['offset']:e['offset'] + len(e['payload'])] = e['payload']
    out.parent.mkdir(parents=True, exist_ok=True)
    temporary = out.with_name(out.name + '.tmp')
    temporary.write_bytes(image)
    temporary.replace(out)
    summary = dict(pack=str(out), bytes=total, sounds=len(entries), data_win_sha256=digest,
                   pcm_bytes=sum(e['pcm_bytes'] for e in entries),
                   payload_bytes=sum(len(e['payload']) for e in entries), pack_sha256=_sha256(bytes(image)))
    log('PACK', json.dumps(summary))
    return summary


def row_check(head: bytes, payload: bytes) -> int:
    """A row's check (ctr_adpcm_pack.inc apPrepare): FNV-1a's step over the little-endian 32-bit words of the row's
    first 24 B (ROW_HEAD: sound ID, bits, offset, length, frames, rate, seam step), then of its payload of whole words
    (the runtime refuses any other row). Each step is a bijection of the state, so one damaged word always changes the
    result, and a payload is never valid under another row (c26rev F3: a flipped sound ID played another sound)."""
    if len(head) != ROW_HEAD.size or len(payload) % 4:
        raise ValueError('A payload must be whole 32-bit words')
    h = 0x811C9DC5
    for (word,) in struct.iter_unpack('<I', head + payload):
        h = ((h ^ word) * 0x01000193) & 0xFFFFFFFF
    return h


def read(path, data_win_sha256=None):
    """Check a pack the way the runtime does (apOpen) and return its rows; raises ValueError."""
    image = Path(path).read_bytes()
    if len(image) < HEADER_BYTES or image[:4] != MAGIC:
        raise ValueError('Not a DTSP pack')
    version, count, total = struct.unpack_from('<III', image, 4)
    if version != VERSION or total != len(image) or image[16:48] != CATALOG_ID:
        raise ValueError('Header, size or catalog id')
    if data_win_sha256 is not None and image[48:112] != data_win_sha256.encode('ascii'):
        raise ValueError('Made from another data.win')
    if not count or count > (total - HEADER_BYTES) // ROW.size:
        raise ValueError('Row count')
    start, rows, previous = HEADER_BYTES + count * ROW.size, [], -1
    for i in range(count):
        sound, bits, _, offset, size, frames, rate, seam, check = ROW.unpack_from(image, HEADER_BYTES + i * ROW.size)
        if (sound <= previous or bits not in (8, 16) or size <= dsp.HEADER_BYTES or size % 4 or offset < start
                or offset > total or size > total - offset or not 0 < rate <= 48000 or not frames):
            raise ValueError(f'Row {i}')
        at = HEADER_BYTES + i * ROW.size
        if row_check(image[at:at + ROW_HEAD.size], image[offset:offset + size]) != check:
            raise ValueError(f'Row {i}: check')  # the runtime plays that sound from its original PCM
        previous = sound
        rows.append(dict(sound=sound, bits=bits, offset=offset, bytes=size, frames=frames, rate=rate, seam=seam))
    return rows


def expected(data_win_sha256, versions=VERSIONS):
    """The release table's record for this data.win digest (size, sha256, sounds), or None when it isn't listed."""
    table = json.loads(Path(versions).read_text(encoding='utf-8'))
    if table.get('schema') != 1 or table.get('game') != 'undertale' or table.get('file') != NAME:
        raise ValueError('Invalid sfxpack versions table')
    found = [r for r in table['packs'] if r['data_sha256'] == data_win_sha256]
    return found[0] if found else None


def verify(summary, versions=VERSIONS):
    """Compare a build() summary with the release table: 'match', 'unlisted', or raise ValueError."""
    record = expected(summary['data_win_sha256'], versions)
    if record is None:
        return 'unlisted'
    got = (summary['bytes'], summary['pack_sha256'], summary['sounds'])
    if got != (record['size'], record['sha256'], record['sounds']):
        raise ValueError('Pack differs from the release table for ' + record['data'] + ': ' + repr(got))
    return 'match'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--data-win', type=Path, required=True, help="The player's UNDERTALE data.win")
    parser.add_argument('--out', type=Path, required=True, help='Output pack (adpcm-sfx.dtsp)')
    parser.add_argument('--work', type=Path, help='Encode cache (default: <out>.work beside the output)')
    parser.add_argument('--dsp-library', type=Path,
                        help='Prebuilt dspadpcm library (dspadpcm.c built with dspadpcm.CFLAGS) for hosts without gcc/clang')
    args = parser.parse_args()
    summary = build(args.data_win, args.out, args.work, library=args.dsp_library)
    try:
        print('RELEASE TABLE', verify(summary), flush=True)
    except ValueError as error:
        args.out.unlink()
        print('ERROR', error, file=sys.stderr, flush=True)
        sys.exit(1)


if __name__ == '__main__':
    main()
