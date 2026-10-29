"""Report existing DSP-on baseline logs without inventing missing counters."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SCENES = ('flowey', 'asgore', 'mettaton', 'snowpuzz')
TIME = re.compile(r'\[app \+([0-9.]+) ms\]')
AUDIO = re.compile(r'cache ([0-9.]+) MB; decode thread core (\d+): busy ([0-9.]+)%, ([0-9.]+) ms per audio second')


def report(folder, scene):
    folder = folder / scene
    receipt = json.loads((folder / 'verified.json').read_text())
    assert receipt['complete'] and receipt['dsp'] and receipt['async_fs']
    assert receipt['speed_limit_percent'] == 100 and receipt['model'] == 'o3ds'
    raw = (folder / 'log.txt').read_bytes()
    assert hashlib.sha256(raw).hexdigest() == receipt['log_sha256']
    text = raw.decode('utf-8', errors='replace')
    ready = next(line for line in text.splitlines() if f'Bench: {scene} ready at frame' in line)
    end = next(line for line in text.splitlines() if f'Bench: {scene} measured frames' in line)
    fps = float(re.search(r'fps=([0-9.]+)', (folder / 'benchmark.txt').read_text())[1])
    start_ms = float(TIME.search(ready)[1]) + 90 * 1000 / fps
    end_ms = float(TIME.search(end)[1])
    samples, windows, previous_ms = [], [], None
    for line in text.splitlines():
        match = AUDIO.search(line)
        if not match:
            continue
        timestamp = float(TIME.search(line)[1])
        cache, core, busy, decode = map(float, match.groups())
        if start_ms <= timestamp <= end_ms:
            samples.append(cache)
            if previous_ms is not None and previous_ms >= start_ms:
                windows.append(dict(core=int(core), busy_percent=busy,
                                    decode_ms_per_audio_second=decode,
                                    busy_ms_per_game_frame=busy / 100 * 1000 / fps))
        previous_ms = timestamp
    assert samples and windows, f'No complete audio windows in {scene}'
    return dict(scene=scene, log_sha256=receipt['log_sha256'], fps=fps,
                sfx_occupied_mib_range=[min(samples), max(samples)],
                sfx_sample_resolution_mib=0.01,
                sfx_payload_saving_upper_estimate_mib=max(samples)*5/7,
                pcm_stream_linear_mib=0.5, read_ahead_linear_mib=1.0,
                decode_ms_per_audio_second_range=[min(w['decode_ms_per_audio_second'] for w in windows),
                                                  max(w['decode_ms_per_audio_second'] for w in windows)],
                busy_ms_per_game_frame_range=[min(w['busy_ms_per_game_frame'] for w in windows),
                                              max(w['busy_ms_per_game_frame'] for w in windows)],
                exact_decode_ms_per_game_frame=None, decoder_heap_bytes=None,
                complete_audio_windows=len(windows), core=1,
                limitations=['Busy time includes service overhead and I/O waits; it is not CPU execution time.',
                    'Exact decode ticks/frame and decoder heap are absent from these logs.',
                    'SFX saving assumes PCM16; actual PCM8/mono collapse and metadata change it.',
                    'Azahar does not model the Old hardware core-1 cap.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--folder', type=Path, default=ROOT / 'scratch/adpcm/measurements/pcm-old64')
    args = parser.parse_args()
    rows = [report(args.folder, scene) for scene in SCENES]
    output = args.folder / 'summary.json'
    output.write_text(json.dumps(rows, indent=2) + '\n')
    for row in rows:
        print(row['scene'], 'cache', row['sfx_occupied_mib_range'], 'MiB;',
              'decode', row['decode_ms_per_audio_second_range'], 'ms/audio s;',
              'service busy', [round(v, 3) for v in row['busy_ms_per_game_frame_range']], 'ms/frame')
    print('WROTE', output)


if __name__ == '__main__':
    main()
