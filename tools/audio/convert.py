"""Experimental offline conversion of an OGG/WAV file or one embedded sound.

    py -3 -I tools/audio/convert.py --file <own.ogg> --out <private/output> --listen
    py -3 -I tools/audio/convert.py --data-win <own/data.win> --sound <name-or-index> --out <private/output> --listen

No installation, runtime selection or release defaults are changed. FFmpeg
decodes the source; our encoder writes .dtad; the independent reference renders
it to PCM for listening. Output must be private and never committed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import mmap
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dspadpcm as dsp


def chunk_table(data):
    if len(data) < 8 or data[:4] != b'FORM':
        raise ValueError('Expected GameMaker FORM data')
    end = struct.unpack_from('<I', data, 4)[0] + 8
    if end != len(data):
        raise ValueError('FORM size differs from source file')
    cursor, chunks = 8, {}
    while cursor < end:
        if cursor + 8 > end:
            raise ValueError('Truncated chunk header')
        name, length = struct.unpack_from('<4sI', data, cursor)
        begin = cursor + 8
        if length > end - begin or name in chunks:
            raise ValueError('Invalid or duplicate chunk')
        chunks[name] = (begin, length)
        cursor = begin + length
    return chunks


def entry_table(data, start, size):
    if size < 4:
        raise ValueError('Truncated entry table')
    count = struct.unpack_from('<I', data, start)[0]
    if count > (size - 4) // 4:
        raise ValueError('Entry count exceeds chunk')
    pointers = struct.unpack_from(f'<{count}I', data, start + 4)
    if any(p < start + 4 + count*4 or p >= start + size for p in pointers):
        raise ValueError('Entry pointer outside chunk')
    return pointers


def game_string(data, address):
    if address < 4 or address > len(data):
        raise ValueError('String pointer outside source')
    length = struct.unpack_from('<I', data, address - 4)[0]
    if length > len(data) - address:
        raise ValueError('String exceeds source')
    return data[address:address+length].decode('utf-8')


def embedded(path, selector):
    """Use the same SOND group/audio indices as the existing game loader."""
    path = Path(path)
    with path.open('rb') as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
        chunks = chunk_table(data)
        sond, size = chunks[b'SOND']
        sounds = entry_table(data, sond, size)
        matches = []
        for index, pointer in enumerate(sounds):
            if pointer + 36 > sond + size:
                raise ValueError('Truncated SOND entry')
            name = game_string(data, struct.unpack_from('<I', data, pointer)[0])
            if str(index) == selector or name == selector:
                matches.append((index, name, pointer))
        if len(matches) != 1:
            raise ValueError(f'Expected one sound for {selector!r}; found {len(matches)}')
        index, name, pointer = matches[0]
        flags = struct.unpack_from('<I', data, pointer + 4)[0]
        if flags & 0x64 == 0x64 and flags & 3 == 0:
            raise ValueError('External sound: use its source file, not a stale AUDO index')
        group, audio = struct.unpack_from('<ii', data, pointer + 28)
        if group < 0 or audio < 0:
            raise ValueError('Sound has no embedded audio entry')
        source_hash = hashlib.sha256(data).hexdigest()
        if group == 0:
            audo, size = chunks[b'AUDO']
            entries = entry_table(data, audo, size)
            if audio >= len(entries):
                raise ValueError('Audio index outside AUDO')
            start = entries[audio]
            length = struct.unpack_from('<I', data, start)[0]
            if length > audo + size - start - 4:
                raise ValueError('Audio blob exceeds AUDO')
            blob = data[start+4:start+4+length]
        else:
            group_path = path.with_name(f'audiogroup{group}.dat')
            with group_path.open('rb') as group_file, mmap.mmap(group_file.fileno(), 0, access=mmap.ACCESS_READ) as group_data:
                audo, size = chunk_table(group_data)[b'AUDO']
                entries = entry_table(group_data, audo, size)
                if audio >= len(entries):
                    raise ValueError('Audio index outside audio group')
                start = entries[audio]
                length = struct.unpack_from('<I', group_data, start)[0]
                if length > audo + size - start - 4:
                    raise ValueError('Audio blob exceeds audio group')
                blob = group_data[start+4:start+4+length]
    return blob, dict(data_win=str(path.resolve()), data_win_sha256=source_hash,
                      sound_index=index, sound_name=name, audio_group=group, audio_index=audio)


def vorbis_frames(blob):
    """The single logical stream's end granule, as the runtime's length probe uses."""
    if blob[:4] != b'OggS':
        return None
    cursor, serial, last = 0, None, None
    while cursor < len(blob):
        if cursor + 27 > len(blob) or blob[cursor:cursor+4] != b'OggS' or blob[cursor+4] != 0:
            raise ValueError('Invalid Ogg page')
        stream = struct.unpack_from('<I', blob, cursor+14)[0]
        if serial is not None and stream != serial:
            raise ValueError('Chained/multiplexed Ogg is not supported by this prototype')
        serial = stream
        segments = blob[cursor+26]
        if cursor + 27 + segments > len(blob):
            raise ValueError('Truncated Ogg lacing table')
        size = sum(blob[cursor+27:cursor+27+segments])
        if size > len(blob) - cursor - 27 - segments:
            raise ValueError('Truncated Ogg payload')
        granule = struct.unpack_from('<Q', blob, cursor+6)[0]
        if granule != 0xFFFFFFFFFFFFFFFF:
            last = granule
        cursor += 27 + segments + size
    if last is None:
        raise ValueError('Ogg source has no sample count')
    return last


def convert(source, out, listen=False, *, blob=None, identity=None, lib=None):
    out = Path(out)
    # A library/build artifact must never overwrite its source or a game file.
    outputs = [out.with_suffix(ext) for ext in ('.dtad', '.json', '.original.wav', '.adpcm.wav')]
    if source is not None and Path(source).resolve() in [p.resolve() for p in outputs]:
        raise ValueError('Output would overwrite source')
    out.parent.mkdir(parents=True, exist_ok=True)
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        raise RuntimeError('FFmpeg is required for experimental source decoding')
    with tempfile.TemporaryDirectory(prefix='dtad-', dir=out.parent) as temporary:
        temporary = Path(temporary)
        if blob is not None:
            source = temporary / 'source.audio'
            source.write_bytes(blob)
        else:
            source = Path(source)
            with source.open('rb') as file:
                blob = file.read()
            identity = dict(file=str(source.resolve()))
        decoded = temporary / 'source.wav'
        subprocess.run([ffmpeg, '-v', 'error', '-nostdin', '-y', '-i', str(source),
                        '-map', '0:a:0', '-vn', '-c:a', 'pcm_s16le', str(decoded)],
                       stdin=subprocess.DEVNULL, check=True, timeout=120)
        with wave.open(str(decoded), 'rb') as wav:
            channels, rate, frames = wav.getnchannels(), wav.getframerate(), wav.getnframes()
            if wav.getsampwidth() != 2 or channels not in (1, 2):
                raise ValueError('Source must decode to mono/stereo PCM16')
            original = wav.readframes(frames)
        row = dsp.encode(original, rate, channels, out.with_suffix('.dtad'), lib)
        # Always validate the entire encoded stream and every stored history.
        rendered, check_rate, check_channels = dsp.read(out.with_suffix('.dtad'))
        if (check_rate, check_channels, len(rendered)) != (rate, channels, len(original)):
            raise ValueError('ADPCM render changed source rate, channels or sample count')
        a, b = dsp.pcm_array(original), dsp.pcm_array(rendered)
        signal = sum(x*x for x in a)
        error = sum((x-y)**2 for x, y in zip(a, b))
        import math
        row.update(source=identity, source_sha256=hashlib.sha256(blob).hexdigest(),
                   pcm_sha256=hashlib.sha256(original).hexdigest(),
                   rendered_sha256=hashlib.sha256(rendered).hexdigest(),
                   rms_error=math.sqrt(error / len(a)),
                   snr_db=10*math.log10(signal / error) if signal and error else None,
                   peak_error=max(abs(x-y) for x, y in zip(a, b)),
                   duration_seconds=frames / rate, approved=False,
                   source_declared_frames=vorbis_frames(blob),
                   timing_eligible=vorbis_frames(blob) in (None, frames),
                   source_decoder='FFmpeg PCM16; runtime Tremor parity still pending')
        if listen:
            dsp.write_wav(out.with_suffix('.original.wav'), original, rate, channels)
            dsp.write_wav(out.with_suffix('.adpcm.wav'), rendered, rate, channels)
        out.with_suffix('.json').write_text(json.dumps(row, indent=2) + '\n', encoding='utf-8')
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--file', type=Path)
    group.add_argument('--data-win', type=Path)
    parser.add_argument('--sound', help='Embedded SOND index or exact name')
    parser.add_argument('--out', type=Path, required=True, help='Private output stem')
    parser.add_argument('--listen', action='store_true')
    args = parser.parse_args()
    if args.data_win:
        if args.sound is None:
            parser.error('--data-win needs --sound')
        blob, identity = embedded(args.data_win, args.sound)
        row = convert(None, args.out, args.listen, blob=blob, identity=identity)
    else:
        row = convert(args.file, args.out, args.listen)
    print(json.dumps({k: row[k] for k in ('frames', 'rate_hz', 'channels', 'pcm_bytes', 'adpcm_bytes', 'snr_db', 'approved')}, indent=2))


if __name__ == '__main__':
    main()
