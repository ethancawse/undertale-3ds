"""Build private, content-addressed DSP-ADPCM assets from the player's files.

Prototype only: the manifest explicitly disables runtime/release selection.
Every unsupported or ineligible source keeps a fallback record. Run with -I.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import mmap
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import convert
import dspadpcm as dsp
import mono_sfx as mono


def digest(data):
    return hashlib.sha256(data).hexdigest()


def catalog(path):
    """Current WAD16/17 SOND layout; deleted slots retain their indices."""
    with path.open('rb') as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
        chunks = convert.chunk_table(data)
        if b'SOND' not in chunks:
            return []
        begin, size = chunks[b'SOND']
        if size < 4:
            raise ValueError('Truncated SOND table')
        count = struct.unpack_from('<I', data, begin)[0]
        if count > (size - 4) // 4:
            raise ValueError('SOND count exceeds chunk')
        pointers = struct.unpack_from(f'<{count}I', data, begin + 4)
        rows = []
        for index, pointer in enumerate(pointers):
            if pointer == 0:
                rows.append(dict(sound_index=index, status='absent'))
                continue
            if pointer < begin + 4 + count * 4 or pointer + 36 > begin + size:
                raise ValueError('SOND entry exceeds chunk')
            name, flags, kind, file = struct.unpack_from('<4I', data, pointer)
            group, audio = struct.unpack_from('<ii', data, pointer + 28)
            if flags & 0x64 != 0x64:
                group = 0  # legacy preload word, as in the runtime parser
            rows.append(dict(sound_index=index, sound_name=convert.game_string(data, name),
                flags=flags, source_file=convert.game_string(data, file), audio_group=group, audio_index=audio,
                embedded=bool(flags & 3) and group >= 0 and audio >= 0))
        return rows


def embedded(path, row):
    source = path if row['audio_group'] == 0 else path.with_name(f'audiogroup{row["audio_group"]}.dat')
    with source.open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        begin, size = convert.chunk_table(data)[b'AUDO']
        pointers = convert.entry_table(data, begin, size)
        index = row['audio_index']
        if index >= len(pointers):
            raise ValueError('Audio index exceeds AUDO')
        pointer = pointers[index]
        if pointer + 4 > begin + size:
            raise ValueError('Truncated AUDO entry')
        length = struct.unpack_from('<I', data, pointer)[0]
        if length > begin + size - pointer - 4:
            raise ValueError('Audio data exceeds AUDO')
        return data[pointer + 4:pointer + 4 + length]


def resolve_external(name, data_win, root):
    relative = PurePosixPath(name.replace('\\', '/'))
    if relative.is_absolute() or '..' in relative.parts or ':' in name:
        raise ValueError('External sound path escapes the game folder')
    for folder in (data_win.parent, root):
        path = (folder / Path(relative)).resolve()
        if not path.is_relative_to(root):
            raise ValueError('External sound link escapes the game folder')
        if path.is_file():
            return path
    raise FileNotFoundError(name)


def encoder_signature(ffmpeg_version, mono_sfx=False):
    sources = b''.join((Path(__file__).parent / name).read_bytes() for name in
        ('dspadpcm.c', 'dspadpcm.py', 'convert.py', 'prepare.py', 'mono_sfx.py'))
    return digest(sources + ffmpeg_version.encode() + f'\0mono_sfx={int(mono_sfx)}'.encode())


def prepare(root, out, *, lib=None, mono_sfx=False):
    root, out = Path(root).resolve(), Path(out).resolve()
    if not root.is_dir() or out.is_relative_to(root) or root.is_relative_to(out):
        raise ValueError('Output and game input trees must be separate')
    data_files = sorted(root.rglob('data.win'))
    if not data_files:
        raise ValueError('No data.win found')
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        raise RuntimeError('FFmpeg is required for experimental conversion')
    ffmpeg_version = subprocess.check_output([ffmpeg, '-version'], stdin=subprocess.DEVNULL,
        timeout=10, text=True).splitlines()[0]
    out.mkdir(parents=True, exist_ok=True)
    (out / 'objects').mkdir(exist_ok=True)
    lib = lib or dsp.native(out / 'host')
    signature = encoder_signature(ffmpeg_version, mono_sfx)
    manifest = dict(version=1, approved=False, runtime_enabled=False, encoder_signature=signature,
                    ffmpeg_version=ffmpeg_version, mono_sfx=mono_sfx,
                    data_files=[], sounds=[], files=[], objects={})
    counts = dict(encoded=0, reused=0, fallback=0)

    def encode(blob, identity, project=False):
        if blob[:4] not in (b'RIFF', b'OggS'):
            raise ValueError('Source is not a runtime-supported WAV/Ogg sound')
        source_sha = digest(blob)
        if project and mono.project(blob) is None:
            raise ValueError('Not an exact dual-mono WAV; retain original PCM')
        key = mono.object_key(blob) if project else source_sha
        if key in manifest['objects']:
            return manifest['objects'][key]
        if shutil.disk_usage(out).free < 1024**3:
            raise RuntimeError('Need at least 1 GiB free during audio conversion')
        stem = out / 'objects' / key
        metadata, payload = stem.with_suffix('.json'), stem.with_suffix('.dtad')
        row = None
        if metadata.is_file() and payload.is_file():
            try:
                cached = json.loads(metadata.read_text())
                if (cached.get('encoder_signature') == signature and cached.get('source_sha256') == source_sha
                        and cached.get('sha256') == digest(payload.read_bytes())):
                    row = cached
                    counts['reused'] += 1
            except (ValueError, OSError):
                pass
        if row is None:
            row = mono.encode(blob, stem, lib=lib) if project else convert.convert(
                None, stem, blob=blob, identity=identity, lib=lib)
            row['encoder_signature'] = signature
            metadata.write_text(json.dumps(row, indent=2) + '\n')
            counts['encoded'] += 1
        result = dict(source_sha256=source_sha, object_key=key, object='objects/' + key + '.dtad',
                      frames=row['frames'], rate_hz=row['rate_hz'], channels=row['channels'],
                      pcm_bytes=row['pcm_bytes'], adpcm_bytes=row['adpcm_bytes'],
                      sha256=row['sha256'], timing_eligible=row['timing_eligible'],
                      status='converted' if row['timing_eligible'] else 'fallback',
                      reason=None if row['timing_eligible'] else 'Source end granule and decoded frame count differ')
        if project:
            result.update(projection=row['projection'], source_channels=row['source_channels'],
                source_bits=row['source_bits'], seam_max_step=row['seam_max_step'])
        manifest['objects'][key] = result
        return result

    def convert_or_fallback(load, identity, embedded=False):
        try:
            blob = load()
            row = encode(blob, identity, project=mono_sfx and embedded and blob[:4] == b'RIFF')
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            row = dict(status='fallback', reason=str(error))
        if row['status'] == 'fallback':
            counts['fallback'] += 1
            print('FALLBACK', identity, row['reason'], flush=True)
        return row

    for data_win in data_files:
        if not data_win.resolve().is_relative_to(root):
            raise ValueError('data.win link escapes game root')
        source_hash = digest(data_win.read_bytes())
        source_path = data_win.relative_to(root).as_posix()
        sounds = catalog(data_win)
        manifest['data_files'].append(dict(path=source_path, sha256=source_hash,
                                           bytes=data_win.stat().st_size, sound_count=len(sounds)))
        for sound in sounds:
            target = dict(data_win_sha256=source_hash, **sound)
            if sound.get('status') == 'absent':
                manifest['sounds'].append(target)
                continue
            print('SOUND', source_path, sound['sound_index'], sound['sound_name'], flush=True)
            identity = dict(data_win=source_path, data_win_sha256=source_hash,
                            sound_index=sound['sound_index'], sound_name=sound['sound_name'])
            if sound['embedded']:
                load = lambda: embedded(data_win, sound)
            else:
                load = lambda: resolve_external(sound['source_file'], data_win, root).read_bytes()
            target.update(convert_or_fallback(load, identity, sound['embedded']))
            manifest['sounds'].append(target)
    for file in sorted(root.rglob('*.ogg')):
        if not file.resolve().is_relative_to(root):
            raise ValueError('Ogg link escapes game root')
        relative = file.relative_to(root).as_posix()
        print('FILE', relative, flush=True)
        result = convert_or_fallback(file.read_bytes, dict(file=relative))
        manifest['files'].append(dict(path=relative, **result))
    manifest['counts'] = counts
    temporary = out / 'manifest.json.tmp'
    temporary.write_text(json.dumps(manifest, indent=2) + '\n')
    temporary.replace(out / 'manifest.json')
    print('PREPARED', counts, 'objects', len(manifest['objects']),
          'runtime_enabled False; approval pending', flush=True)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, required=True, help="Player's game installation")
    parser.add_argument('--out', type=Path, required=True, help='Private build/output tree')
    parser.add_argument('--mono-sfx', action='store_true', help='Project only exact dual-mono embedded WAVs')
    args = parser.parse_args()
    prepare(args.data, args.out, mono_sfx=args.mono_sfx)


if __name__ == '__main__':
    main()
