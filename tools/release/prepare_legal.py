#!/usr/bin/env python3
"""Bundle original legal texts and fixed-size console pages; no game inputs."""
import argparse
import json
from pathlib import Path
import re
import struct

# Reflow: the sources are hard-wrapped near 70-79 columns, so wrapping their lines again at 38 left a short tail row
# ("for") in most paragraphs. A line joins the one before it (one space) only when that one is prose cut by the
# source's wrap: at least JOIN_MIN columns, with a letter or digit, and the line itself is not blank, not a list item,
# a "Key: value" field or a copyright line, has a letter or digit and is indented no deeper than its paragraph's first
# line. Blank lines, centred headings, lists, fields, separators and short lines (names, addresses) keep their breaks.
JOIN_MIN = 40
FIELD = re.compile(r'\s*(?:[A-Z][A-Za-z0-9 ./()-]{0,40}:\s|(?i:copyright\b|\(c\)|\u00a9|spdx-))')
LIST_ITEM = re.compile(r'\s*(?:[-*+\u2022]|\(?[0-9]{1,3}[.)]|\(?[a-zA-Z][.)]|\([ivx]+\))\s')


def split_bytes(line, columns):
    """Rows of at most `columns` UTF-8 bytes, never bisecting a character."""
    rows = []
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
    return rows


def wrap(line, columns):
    """One source line as rows. Only whitespace changes: a line too long for a row breaks at its last space that
    fits (the space itself is dropped) and a word longer than a row is split; a centred heading too long only for its
    indent is centred in the row instead."""
    size = len(line.encode('utf-8'))
    if size <= columns:
        return [line.encode('utf-8')]
    body = line.lstrip(' ')
    indent = len(line) - len(body)
    if indent >= 8 and len(body.encode('utf-8')) <= columns:
        return [(' ' * ((columns - len(body.encode('utf-8'))) // 2) + body).encode('utf-8')]
    rows = []
    while len(line.encode('utf-8')) > columns:
        cut = -1
        used = 0
        for i, character in enumerate(line):
            if character == ' ' and i > 0 and line[:i].strip():
                cut = i
            used += len(character.encode('utf-8'))
            if used > columns:
                break
        if cut <= 0:
            head = split_bytes(line, columns)[0]
            rows.append(head)
            line = line[len(head.decode('utf-8')):]
        else:
            rows.append(line[:cut].encode('utf-8'))
            line = line[cut + 1:]
    rows.append(line.encode('utf-8'))
    return rows


def indent_of(line):
    return len(line) - len(line.lstrip(' '))


def reflow(lines):
    """Join each paragraph's hard-wrapped lines into one line; only whitespace changes."""
    out = []
    first = 0  # indent of the paragraph's first line
    joinable = False
    for line in lines:
        body = line.strip(' ')
        prose = any(character.isalnum() for character in body)
        if (joinable and body and prose and not LIST_ITEM.match(line) and not FIELD.match(line) and indent_of(line) <= first):
            out[-1] = out[-1].rstrip(' ') + ' ' + body
        else:
            out.append(line)
            first = indent_of(line)
        tail = line.rstrip(' ')
        joinable = len(tail) >= JOIN_MIN and prose
    return out


def paginate(data, columns, lines):
    if not 1 <= columns <= 38 or not 1 <= lines <= 22:
        raise ValueError('legal page dimensions exceed the settings console')
    text = data.decode('utf-8')
    if '\0' in text:
        raise ValueError('NUL in legal text')
    rows = []
    for line in reflow(text.expandtabs(4).splitlines()):
        rows.extend(wrap(line, columns))
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
