"""Stage prepared DSP-ADPCM objects and validated lookup indexes privately.

Only reads the player's installation. Output is separate from both the input
and converter cache. This prepares SD content; the runtime adapter selects it.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import mmap
from pathlib import Path, PurePosixPath
import shutil
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prepare
import convert
import mono_sfx as mono

HEADER = 128
RECORD = 128
LIMIT = 4 * 1024**2
VALID, EMBEDDED, DUAL_MONO = 1, 2, 4
PROJECTED_MONO = 8


def relative(name):
    path = PurePosixPath(name)
    if (not name or path.is_absolute() or '\\' in name or ':' in name or
            any(c in ('', '.', '..') for c in name.split('/')) or
            any(ord(c) < 32 or ord(c) == 127 for c in name)):
        raise ValueError('Invalid relative audio path')
    return path


def below(root, name):
    path = (root / Path(relative(name))).resolve()
    if not path.is_relative_to(root):
        raise ValueError('Audio link escapes its root')
    return path


def source_span(root, data_win, row):
    if not row['embedded']:
        file = prepare.resolve_external(row['source_file'], data_win, root)
        return file, 0, file.read_bytes()
    file = data_win if row['audio_group'] == 0 else data_win.with_name(f'audiogroup{row["audio_group"]}.dat')
    if not file.resolve().is_relative_to(root):
        raise ValueError('Audio group link escapes game root')
    with file.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        begin, size = convert.chunk_table(data)[b'AUDO']
        pointers = convert.entry_table(data, begin, size)
        index = row['audio_index']
        if not 0 <= index < len(pointers):
            raise ValueError('Audio index exceeds AUDO')
        offset = pointers[index]
        if offset + 4 > begin + size:
            raise ValueError('Truncated audio entry')
        length = struct.unpack_from('<I', data, offset)[0]
        offset += 4
        if length > begin + size - offset:
            raise ValueError('Audio span exceeds AUDO')
        return file, offset, data[offset:offset + length]


def payload_info(data, row):
    if len(data) < HEADER or data[:4] != b'DTAD':
        raise ValueError('Truncated or unrecognized prepared payload')
    version, header, rate, channels = struct.unpack_from('<4I', data, 4)
    frames, block_frames, blocks, flags = struct.unpack_from('<Q3I', data, 20)
    if (version != 1 or header != HEADER or block_frames != 2044 or flags or
            not 0 < frames <= 0xffffffff or not 0 < rate <= 192000 or channels not in (1, 2) or
            blocks != (frames + 2043) // 2044 or
            (frames, rate, channels, len(data)) !=
            (row['frames'], row['rate_hz'], row['channels'], row['adpcm_bytes'])):
        raise ValueError('Prepared payload metadata differs')
    last = frames - (blocks - 1) * 2044
    if len(data) != HEADER + (blocks - 1) * channels * 1176 + channels * (8 + ((last + 13) // 14) * 8):
        raise ValueError('Prepared payload length differs')
    for ch in range(channels):
        if any(data[72 + ch * 40:80 + ch * 40]):
            raise ValueError('Nonzero reserved ADPCM context')
    if any(data[40 + channels * 40:HEADER]):
        raise ValueError('Nonzero reserved ADPCM header')
    dual_mono = channels == 2 and data[40:80] == data[80:120]
    at = HEADER
    for first in range(0, frames, 2044):
        size = 8 + ((min(2044, frames - first) + 13) // 14) * 8
        if dual_mono and data[at:at + size] != data[at + size:at + size * 2]:
            dual_mono = False
        at += channels * size
    return dual_mono


def make_index(data_sha, sounds, files, count):
    if len(sounds) != count or count > 65535 or len(files) > 65535:
        raise ValueError('Audio index counts differ')
    strings = bytearray()
    offsets = {}
    records = bytearray()
    for row in sounds + files:
        record = bytearray(RECORD)
        if row is not None:
            name = str(relative(row['path']))
            if name not in offsets:
                offsets[name] = len(strings)
                strings.extend(name.encode('utf-8') + b'\0')
            struct.pack_into('<6IQ2I', record, 0, row['flags'], row['frames'], row['rate_hz'],
                row['channels'], row['adpcm_bytes'], offsets[name], row['source_offset'],
                row['source_bytes'], row.get('source_channels', row['channels']))
            record[40:72] = bytes.fromhex(row.get('object_key', row['source_sha256']))
            record[72:92] = bytes.fromhex(row['payload_sha1'])
            record[92:112] = bytes.fromhex(row['source_sha1'])
            if row['flags'] & PROJECTED_MONO:
                struct.pack_into('<QII', record, 112, row['source_file_bytes'], row['source_bits'], row['seam_max_step'] + 1)
        records.extend(record)
    body = records + strings
    header = bytearray(HEADER)
    struct.pack_into('<4s7I', header, 0, b'DTAI', 1, HEADER, RECORD, count, len(files), len(body), len(strings))
    header[32:64] = bytes.fromhex(data_sha)
    header[64:84] = hashlib.sha1(body).digest()
    if HEADER + len(body) > LIMIT:
        raise ValueError('Audio index exceeds 4 MiB')
    return bytes(header + body)


def stage(root, prepared, out):
    root, prepared, out = (Path(p).resolve() for p in (root, prepared, out))
    for source in (root, prepared):
        if out.is_relative_to(source) or source.is_relative_to(out):
            raise ValueError('Staging output must be separate from game and prepared inputs')
    manifest = json.loads((prepared / 'manifest.json').read_text())
    if manifest.get('version') != 1 or not manifest.get('data_files'):
        raise ValueError('Unsupported prepared manifest')
    if prepare.encoder_signature(manifest['ffmpeg_version'], manifest.get('mono_sfx', False)) != manifest['encoder_signature']:
        raise ValueError('Prepared encoder changed; convert again')
    marker = below(out, '.audio-stage-v1')
    if out.exists() and any(out.iterdir()) and (not marker.is_file() or marker.read_bytes() != b'DTAD staging v1\n'):
        raise ValueError('Output is not an owned ADPCM staging folder')
    out.mkdir(parents=True, exist_ok=True)
    marker.write_bytes(b'DTAD staging v1\n')
    for directory in ('objects', 'index'):
        path = below(out, directory)
        path.mkdir(exist_ok=True)
    objects = {}
    staged = {}

    def record(row, file, offset, blob, embedded=False):
        if row.get('status') != 'converted' or not row.get('timing_eligible'):
            return None
        source_key = hashlib.sha256(blob).hexdigest()
        if source_key != row['source_sha256']:
            raise ValueError('Prepared source changed; convert again')
        projected = row.get('projection') is not None
        key = row.get('object_key', source_key)
        expected_key = mono.object_key(blob) if projected else source_key
        if key != expected_key:
            raise ValueError('Prepared object policy identity differs')
        if projected:
            projection = mono.project(blob)
            if (not embedded or projection is None or row['projection'] != mono.POLICY or
                    (row['frames'], row['rate_hz'], row['channels'], row.get('source_channels'), row.get('source_bits')) !=
                    (projection.frames, projection.rate_hz, 1, 2, projection.source_bits)):
                raise ValueError('Prepared mono projection metadata differs from source')
        identity = tuple(row[name] for name in ('frames', 'rate_hz', 'channels', 'adpcm_bytes', 'sha256')) + (row.get('seam_max_step'),)
        if key not in objects:
            path = below(prepared, row['object'])
            payload = path.read_bytes()
            if hashlib.sha256(payload).hexdigest() != row['sha256']:
                raise ValueError('Prepared payload hash differs')
            dual = payload_info(payload, row)
            if projected:
                rendered, _, _ = mono.dsp.read(path)
                if row.get('seam_max_step') != mono.seam_max_step(rendered):
                    raise ValueError('Prepared mono seam statistic differs')
            target = below(out, 'objects/' + key + '.dtad')
            if not target.is_file() or hashlib.sha256(target.read_bytes()).hexdigest() != row['sha256']:
                if shutil.disk_usage(out).free < len(payload) + 64 * 1024**2:
                    raise RuntimeError('Need payload size plus 64 MiB free to stage audio')
                temporary = below(out, 'objects/' + key + '.dtad.tmp')
                temporary.write_bytes(payload)
                temporary.replace(target)
            objects[key] = (hashlib.sha1(payload).hexdigest(), dual, identity)
            staged[key] = dict(bytes=len(payload), sha256=row['sha256'])
        payload_sha1, dual, known_identity = objects[key]
        if identity != known_identity:
            raise ValueError('Inconsistent prepared object aliases')
        return dict(row, flags=VALID | (EMBEDDED if embedded else 0) | (DUAL_MONO if dual else 0) |
            (PROJECTED_MONO if projected else 0), source_file_bytes=file.stat().st_size,
            path=file.relative_to(root).as_posix(), source_offset=offset, source_bytes=len(blob),
            source_sha1=hashlib.sha1(blob).hexdigest(), payload_sha1=payload_sha1)

    files = []
    for row in manifest['files']:
        if row.get('status') != 'converted' or not row.get('timing_eligible'):
            files.append(None)
            continue
        file = below(root, row['path'])
        files.append(record(row, file, 0, file.read_bytes()))
    indexes = []
    for data in manifest['data_files']:
        data_win = below(root, data['path'])
        if hashlib.sha256(data_win.read_bytes()).hexdigest() != data['sha256']:
            raise ValueError('Prepared data.win changed; convert again')
        rows = [r for r in manifest['sounds'] if r['data_win_sha256'] == data['sha256']]
        if len(rows) != data['sound_count'] or [r['sound_index'] for r in rows] != list(range(data['sound_count'])):
            raise ValueError('Prepared SOND slots differ')
        sounds = []
        for row in rows:
            if row.get('status') != 'converted' or not row.get('timing_eligible'):
                sounds.append(None)
                continue
            file, offset, blob = source_span(root, data_win, row)
            sounds.append(record(row, file, offset, blob, row['embedded']))
        index = make_index(data['sha256'], sounds, files, data['sound_count'])
        name = 'index/' + data['sha256'] + '.dtai'
        temporary = below(out, name + '.tmp')
        temporary.write_bytes(index)
        temporary.replace(below(out, name))
        indexes.append(dict(data_win=data['path'], data_win_sha256=data['sha256'], path=name,
            sha256=hashlib.sha256(index).hexdigest(), bytes=len(index), sound_slots=len(sounds),
            converted_sounds=sum(row is not None for row in sounds),
            file_slots=len(files), converted_files=sum(row is not None for row in files)))
    result = dict(version=1, runtime_selected=False, indexes=indexes, objects=staged,
                  object_bytes=sum(row['bytes'] for row in staged.values()))
    temporary = below(out, 'stage.json.tmp')
    temporary.write_text(json.dumps(result, indent=2) + '\n')
    temporary.replace(below(out, 'stage.json'))
    print('STAGED', len(indexes), 'indexes;', len(staged), 'objects;', result['object_bytes'], 'B;',
          'runtime adapter selection pending', flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    stage(args.data, args.prepared, args.out)


if __name__ == '__main__':
    main()
