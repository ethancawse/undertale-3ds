"""Gate log-window selection and frame weighting with synthetic counters."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import report_metrics as metrics


class ReportTests(unittest.TestCase):
    def fixture(self, root, overflow=0):
        folder = root / 'flowey'
        folder.mkdir()
        (folder / 'benchmark.txt').write_text('fps=30.000\n')
        lines = ['[app +1000.0 ms] Bench: flowey ready at frame 1']
        for stamp, frames, per_frame in [(2000, 150, 9), (5000, 150, 8), (10000, 100, 1), (15000, 150, 2)]:
            ticks = round(metrics.TICKS_PER_MS * frames * per_frame)
            lines += [f'[app +{stamp}.0 ms] [audiometrics] decode {ticks} ticks / {frames} game frames = {per_frame:.6f} ms/frame; core 1',
                      f'[app +{stamp}.0 ms] [audiometrics] sfx 1000000 B (0 PCM8, 1000000 PCM16); adpcm_same_channels 287000 B; '
                      'stream_linear 524288 B; stream_context_heap 24000 B; decoder_heap_usable 150000 B; '
                      f'decoder_linear_requested 0 B; tracked 100 allocations; overflow {overflow}',
                      f'[app +{stamp}.0 ms] [audiometrics] sfx_metadata 5000 heap B 0 linear B; audo_blobs 0 heap B 0 linear B']
        lines.append('[app +16000.0 ms] Bench: flowey measured frames 700')
        raw = ('\n'.join(lines) + '\n').encode()
        (folder / 'log.txt').write_bytes(raw)
        (folder / 'verified.json').write_text(json.dumps(dict(complete=True, dsp=True, async_fs=True,
            model='o3ds', speed_limit_percent=100, binary_sha256='0'*64, log_sha256=hashlib.sha256(raw).hexdigest())))
        return folder

    def test_excludes_partial_window_and_weights_game_frames(self):
        with tempfile.TemporaryDirectory(dir=metrics.ROOT / 'scratch/adpcm') as temporary:
            root = Path(temporary)
            self.fixture(root)
            row = metrics.report(root, 'flowey')
            self.assertEqual(row['complete_windows'], 2)
            self.assertEqual(row['game_frames'], 250)
            self.assertAlmostEqual(row['decode_ms_per_game_frame'], 1.6, places=6)
            self.assertEqual(len(row['samples']), 3)
            self.assertEqual(row['ranges']['sfx_estimated_saving_bytes'], [713000, 713000])

    def test_rejects_overflow(self):
        with tempfile.TemporaryDirectory(dir=metrics.ROOT / 'scratch/adpcm') as temporary:
            root = Path(temporary)
            self.fixture(root, overflow=1)
            with self.assertRaisesRegex(AssertionError, 'overflow'):
                metrics.report(root, 'flowey')

    def test_rejects_changed_evidence(self):
        with tempfile.TemporaryDirectory(dir=metrics.ROOT / 'scratch/adpcm') as temporary:
            root = Path(temporary)
            folder = self.fixture(root)
            with (folder / 'log.txt').open('a') as file:
                file.write('changed\n')
            with self.assertRaises(AssertionError):
                metrics.report(root, 'flowey')


if __name__ == '__main__':
    unittest.main()
