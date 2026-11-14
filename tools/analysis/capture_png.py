"""Lossless capture writer and explicit, non-destructive BMP-folder migration.

Pixel MD5 means RGB bytes in top-to-bottom row order, with no stride/padding.
File SHA-256 remains separate: encoding bytes are not a pixel comparison.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import zlib

_spec = importlib.util.spec_from_file_location('capture_picture_check', Path(__file__).with_name('picture_check.py'))
_pictures = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_pictures)


def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_png(path: Path, width: int, height: int, rows: list[bytes]) -> dict:
    """Write a NEW PNG, verify decoded pixels, append the adjacent pixel list."""
    if path.suffix.lower() != '.png' or path.exists():
        raise ValueError(f'Expected a new .png path: {path}')
    if width < 1 or height < 1 or len(rows) != height or any(len(row) != width * 3 for row in rows):
        raise ValueError('Expected complete RGB8 rows')
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    packed = (b'\x89PNG\r\n\x1a\n' +
              chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
              chunk(b'IDAT', zlib.compress(b''.join(b'\0' + row for row in rows), 6)) +
              chunk(b'IEND', b''))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(packed)
    decoded = _pictures.read_png(path.read_bytes())
    if decoded != (width, height, rows):
        raise AssertionError(f'PNG pixel mismatch: {path}')
    md5 = hashlib.md5(b''.join(rows)).hexdigest()
    with (path.parent / 'pixel-md5.txt').open('a', encoding='utf-8', newline='\n') as stream:
        stream.write(f'{md5}  {path.name}\n')
    return dict(file_sha256=digest(path), pixel_md5=md5, pixel_format='RGB8-row-major',
                width=width, height=height, pixel_mismatches=0)


def write_bmp_png(path: Path, bmp: bytes) -> dict:
    """Decode a legacy in-memory BMP without ever writing it to disk."""
    return write_png(path, *_pictures.read_bmp(bmp))


def convert_folder(source: Path, destination: Path) -> dict:
    """Mirror a finished folder to a NEW sibling; never remove the source.

    Callers must check writers/KEEP/report references before enqueueing the old
    folder for cleanwatch. Unchanged auxiliary files are copied and hashed.
    """
    source, destination = source.resolve(strict=True), destination.resolve()
    if not source.is_dir() or destination.exists() or source.parent != destination.parent:
        raise ValueError('Destination must be a new sibling of the source folder')
    files = sorted(p for p in source.rglob('*') if p.is_file())
    if not any(p.suffix.lower() == '.bmp' for p in files):
        raise ValueError('Source has no BMP pictures')
    # Refuse name collisions before creating any output (foo.bmp + foo.png).
    names = [p.relative_to(source).with_suffix('.png') if p.suffix.lower() == '.bmp'
             else p.relative_to(source) for p in files]
    if len({str(p).casefold() for p in names}) != len(names):
        raise ValueError('Converted file names collide')
    if any(p.name in ('pixel-md5.txt', 'png-migration.json') for p in files):
        raise ValueError('Source already has PNG migration metadata')
    destination.mkdir()
    rows = []
    for old, name in zip(files, names):
        new = destination / name
        row = dict(source=str(old.relative_to(source)), file=str(name),
                   source_sha256=digest(old), source_bytes=old.stat().st_size)
        if old.suffix.lower() == '.bmp':
            row.update(write_bmp_png(new, old.read_bytes()))
        else:
            new.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(old, new)
            row['file_sha256'] = digest(new)
            if row['file_sha256'] != row['source_sha256']:
                raise AssertionError(f'Auxiliary copy mismatch: {old}')
        row['bytes'] = new.stat().st_size
        rows.append(row)
    report = dict(source=str(source), destination=str(destination), files=rows,
                  pictures=sum('pixel_md5' in r for r in rows), pixel_mismatches=0,
                  source_bytes=sum(r['source_bytes'] for r in rows),
                  destination_payload_bytes=sum(r['bytes'] for r in rows),
                  source_removed=False, cleanup_queued=False)
    (destination / 'png-migration.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    report = convert_folder(args.source, args.destination)
    print(json.dumps({k: v for k, v in report.items() if k != 'files'}, indent=2))


if __name__ == '__main__':
    main()
