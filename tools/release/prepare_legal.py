#!/usr/bin/env python3
"""Bundle original legal texts and fixed-size console pages; no game inputs."""
import argparse
import json
from pathlib import Path
import struct


def paginate(data, columns, lines):
    if not 1 <= columns <= 38 or not 1 <= lines <= 22:
        raise ValueError('legal page dimensions exceed the settings console')
    text = data.decode('utf-8')
    if '\0' in text:
        raise ValueError('NUL in legal text')
    rows = []
    for line in text.expandtabs(4).splitlines():
        # Split Unicode characters rather than bisecting a UTF-8 sequence.
        row = bytearray()
        for character in line:
            encoded = character.encode('utf-8')
            if len(encoded) > columns:
                raise ValueError('legal page is narrower than a UTF-8 character')
            if len(row) + len(encoded) > columns:
                rows.append(bytes(row))
                row.clear()
            row.extend(encoded)
        rows.append(bytes(row))
    if not rows:
        rows.append(b'')
    count = (len(rows) + lines - 1) // lines
    padded = rows + [b''] * (count * lines - len(rows))
    return b'LGL1' + struct.pack('<I', count) + b''.join(
        row.ljust(columns + 1, b'\0') for row in padded)


def write_changed(path, data):
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def prepare(root, output):
    contract = json.loads((root / 'docs/release/build-contract.json').read_text(encoding='utf-8'))
    output.mkdir(parents=True, exist_ok=True)
    for name in ('LICENSE', 'THIRD_PARTY_NOTICES'):
        data = (root / name).read_bytes()
        pages = paginate(data, contract['legal_columns'], contract['legal_lines'])
        write_changed(output / name, data)
        write_changed(output / (name + '.pages'), pages)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.root, args.output)
