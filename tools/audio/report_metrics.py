"""Extract allocation bytes and frame-weighted decode ticks from diagnostic logs."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SCENES = ('flowey', 'asgore', 'mettaton', 'snowpuzz')
STAMP = re.compile(r'\[app \+([0-9.]+) ms\]')
TIMING = re.compile(r'\[audiometrics\] decode (\d+) ticks / (\d+) game frames = ([0-9.]+) ms/frame; core (\d+)')
MEMORY = re.compile(r'\[audiometrics\] sfx (\d+) B \((\d+) PCM8, (\d+) PCM16\); adpcm_same_channels (\d+) B; '
                    r'stream_linear (\d+) B; stream_context_heap (\d+) B; decoder_heap_usable (\d+) B; '
                    r'decoder_linear_requested (\d+) B; tracked (\d+) allocations; overflow (\d+)')
SFX_HEAP = re.compile(r'\[audiometrics\] sfx_metadata (\d+) heap B (\d+) linear B; audo_blobs (\d+) heap B (\d+) linear B')
KEYS = ('sfx_linear_bytes', 'sfx_pcm8_bytes', 'sfx_pcm16_bytes', 'adpcm_same_channels_bytes',
        'stream_linear_bytes', 'stream_context_heap_bytes', 'decoder_heap_usable_bytes',
        'decoder_linear_requested_bytes', 'tracked_allocations', 'overflow')
HEAP_KEYS = ('sfx_metadata_heap_bytes', 'sfx_metadata_linear_bytes', 'audo_blob_heap_bytes', 'audo_blob_linear_bytes')
TICKS_PER_MS = 16756991 * 16 / 1000  # installed libctru: SYSCLOCK_ARM11


def report(folder, scene):
    folder = folder / scene
    receipt = json.loads((folder / 'verified.json').read_text())
    assert receipt['complete'] and receipt['dsp'] and receipt['async_fs']
    assert receipt['model'] == 'o3ds' and receipt['speed_limit_percent'] == 100
    raw = (folder / 'log.txt').read_bytes()
    assert hashlib.sha256(raw).hexdigest() == receipt['log_sha256']
    lines = raw.decode(errors='replace').splitlines()
    ready = next(l for l in lines if f'Bench: {scene} ready at frame' in l)
    end = next(l for l in lines if f'Bench: {scene} measured frames' in l)
    fps = float(re.search(r'fps=([0-9.]+)', (folder / 'benchmark.txt').read_text())[1])
    start_ms = float(STAMP.search(ready)[1]) + 90000 / fps
    end_ms = float(STAMP.search(end)[1])
    samples, windows = [], []
    previous_ms, pending = None, None
    for line in lines:
        timing = TIMING.search(line)
        if timing:
            now_ms = float(STAMP.search(line)[1])
            ticks, frames, printed, core = timing.groups()
            ticks, frames, core = int(ticks), int(frames), int(core)
            assert frames > 0 and core == 1
            calculated = ticks / TICKS_PER_MS / frames
            assert abs(calculated - float(printed)) <= 0.000001
            pending = dict(timestamp_ms=now_ms, decode_ticks=ticks, game_frames=frames,
                           decode_ms_per_game_frame=calculated,
                           complete=previous_ms is not None and previous_ms >= start_ms)
            previous_ms = now_ms
        memory = MEMORY.search(line)
        if memory:
            assert pending is not None, 'Unpaired memory counters'
            row = dict(zip(KEYS, map(int, memory.groups())))
            assert row['overflow'] == 0, 'Allocation accounting overflow invalidates this run'
            assert row['sfx_linear_bytes'] == row['sfx_pcm8_bytes'] + row['sfx_pcm16_bytes']
            pending.update(row)
        heap = SFX_HEAP.search(line)
        if heap:
            assert pending is not None and 'overflow' in pending
            pending.update(zip(HEAP_KEYS, map(int, heap.groups())))
            pending['sfx_estimated_saving_bytes'] = pending['sfx_linear_bytes'] - pending['adpcm_same_channels_bytes']
            if start_ms <= pending['timestamp_ms'] <= end_ms:
                samples.append(pending)
                if pending['complete']: windows.append(pending)
            pending = None
    assert samples and windows, f'No complete measured windows: {scene}'
    ticks = sum(w['decode_ticks'] for w in windows)
    frames = sum(w['game_frames'] for w in windows)
    ranges = {key: [min(s[key] for s in samples), max(s[key] for s in samples)]
              for key in (*KEYS, *HEAP_KEYS, 'sfx_estimated_saving_bytes')}
    return dict(scene=scene, build_sha256=receipt['binary_sha256'], log_sha256=receipt['log_sha256'],
                measured_window_ms=[start_ms, end_ms], complete_windows=len(windows),
                decode_ticks=ticks, game_frames=frames, decode_ms_per_game_frame=ticks / TICKS_PER_MS / frames,
                ranges=ranges, samples=samples,
                limitations=['Decode-call wall ticks include scheduling, I/O and diagnostic overhead; not host CPU execution.',
                    'Azahar does not reproduce the Old hardware core-1 cap.',
                    'SFX estimate keeps current cached channel counts and includes DTAD header/block contexts.',
                    'This is a prospective payload saving, not a measured native ADPCM gain.',
                    'Heap usable bytes exclude malloc headers; linear fallback uses requested byte counts.',
                    'Decoder scope can retain libc allocations; shared preexisting FILE pools and thread stacks are excluded.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--folder', type=Path, default=ROOT / 'scratch/adpcm/measurements/pcm-metrics-old64')
    args = parser.parse_args()
    rows = [report(args.folder, scene) for scene in SCENES]
    (args.folder / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')
    for row in rows:
        r = row['ranges']
        print(row['scene'], f'{row["decode_ms_per_game_frame"]:.6f} ms/game frame',
              'SFX', r['sfx_linear_bytes'], 'B;', 'decoder heap', r['decoder_heap_usable_bytes'], 'B;',
              'SFX prospective saving', [round(b / 2**20, 6) for b in r['sfx_estimated_saving_bytes']], 'MiB')


if __name__ == '__main__':
    main()
