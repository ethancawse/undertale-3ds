"""Verify listening artifacts against their manifest, without rendering again."""
import argparse
import hashlib
import json
from pathlib import Path
import wave


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    folder = parser.parse_args().folder
    manifest = json.loads((folder / 'manifest.json').read_text())
    for name, expected in manifest['encoder_sources_sha256'].items():
        assert hashlib.sha256((Path(__file__).parent / name).read_bytes()).hexdigest() == expected, name
    assert len(manifest['samples']) == 6
    for row in manifest['samples']:
        encoded = folder / (row['stem'] + '.dtad')
        assert encoded.stat().st_size == row['adpcm_bytes']
        assert hashlib.sha256(encoded.read_bytes()).hexdigest() == row['sha256']
        for suffix, key in [('original', 'pcm_sha256'), ('adpcm', 'rendered_sha256')]:
            file = folder / (row['stem'] + '.' + suffix + '.wav')
            with wave.open(str(file), 'rb') as sound:
                assert sound.getsampwidth() == 2
                assert sound.getnframes() == row['frames']
                assert sound.getframerate() == row['rate_hz']
                assert sound.getnchannels() == row['channels']
                digest = hashlib.sha256()
                while block := sound.readframes(65536):
                    digest.update(block)
                assert digest.hexdigest() == row[key], file
        print(row['stem'], f'{row["frames"]} frames', f'{row["rate_hz"]} Hz',
              f'{row["channels"]} channels', 'both WAV hashes verified')
    print('6 pairs verified; manifest approval:', manifest['approved'])


if __name__ == '__main__':
    main()
