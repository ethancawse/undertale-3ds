#!/usr/bin/env python3
"""Bundle original legal texts and fixed-size console pages; no game inputs."""
import argparse
from bisect import bisect_right
from collections import deque
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
# The console's reader has ASCII glyphs only: ctr_legal.c's luiLine draws anything else as '?'. Its pages spell these
# the usual way. The bundled LICENSE and THIRD_PARTY_NOTICES keep the real characters. A new character stops the build
# here (test_prepare_legal.py runs this on the shipped files), so a notice can't reach a console with a '?' in a name.
CONSOLE_SPELLING = {'\u00a9': '(c)', '\u00f6': 'o'}


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


def console_text(text):
    """The text as the reader can draw it: only CONSOLE_SPELLING's characters change."""
    for character, spelling in CONSOLE_SPELLING.items():
        text = text.replace(character, spelling)
    unknown = sorted({character for character in text if ord(character) > 126})
    if unknown:
        raise ValueError('legal text has characters the console cannot draw: ' + ascii(unknown))
    return text


def paginate(data, columns, lines):
    if not 1 <= columns <= 38 or not 1 <= lines <= 22:
        raise ValueError('legal page dimensions exceed the settings console')
    text = console_text(data.decode('utf-8'))
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


_LENGTH_BASE = (3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                67, 83, 99, 115, 131, 163, 195, 227, 258)
_LENGTH_EXTRA = (0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                 4, 4, 4, 4, 5, 5, 5, 5, 0)
_DISTANCE_BASE = (1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                  513, 769, 1025, 1537, 2049, 3073, 4097, 6145)
_DISTANCE_EXTRA = (0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7,
                   8, 8, 9, 9, 10, 10, 11, 11)


def _reverse_bits(value, width):
    result = 0
    for _ in range(width):
        result = result * 2 + (value & 1)
        value >>= 1
    return result


# RFC 1951 fixed trees: canonical Huffman codes go onto the LSB-first wire bit-reversed.
_FIXED_CODES = tuple((_reverse_bits(code, width), width) for code, width in (
    (symbol + 0x30, 8) if symbol < 144 else (symbol - 144 + 0x190, 9) if symbol < 256 else
    (symbol - 256, 7) if symbol < 280 else (symbol - 280 + 0xc0, 8) for symbol in range(288)))
_DISTANCE_CODES = tuple(_reverse_bits(i, 5) for i in range(len(_DISTANCE_BASE)))


def _zlib_fixed(data):
    """One standard zlib stream: fixed-Huffman DEFLATE, greedy 8 KiB LZ77, explicit Adler-32.

    Deterministic by construction: integer operations only, no floats or dict/set iteration, and no hash-seeded
    choice. A dict only retrieves a byte-prefix's insertion-ordered chain. Search the newest 64 positions first;
    longest match wins, nearest wins ties. Insert every consumed position, including within an overlapping match.
    Each NSP1 block starts a fresh stream/window. This must never delegate compression to the host Python zlib.
    """
    output = bytearray(b'\x78\x01')  # RFC 1950: DEFLATE, 32 KiB advertised window, no dictionary, FCHECK valid.
    accumulator = 0
    available = 0

    def emit(value, width):
        nonlocal accumulator, available
        accumulator |= value << available
        available += width
        while available >= 8:
            output.append(accumulator & 255)
            accumulator >>= 8
            available -= 8

    emit(3, 3)  # BFINAL=1, BTYPE=01: one final fixed-Huffman block.
    chains = {}
    position = 0
    while position < len(data):
        end = min(position + 258, len(data))
        best = 2
        previous = 0
        key = data[position:position + 3]
        chain = chains.get(key, ()) if len(key) == 3 else ()
        for at in reversed(chain):
            if position - at > 8192:
                break
            if not data.startswith(data[position:position + best + 1], at):
                continue
            length = best + 1
            while position + length < end and data[at + length] == data[position + length]:
                length += 1
            best, previous = length, at
            if position + best == end:
                break
        step = 1
        if best >= 3:
            step = best
            code = bisect_right(_LENGTH_BASE, best) - 1
            emit(*_FIXED_CODES[257 + code])
            emit(best - _LENGTH_BASE[code], _LENGTH_EXTRA[code])
            distance = position - previous
            code = bisect_right(_DISTANCE_BASE, distance) - 1
            emit(_DISTANCE_CODES[code], 5)
            emit(distance - _DISTANCE_BASE[code], _DISTANCE_EXTRA[code])
        else:
            emit(*_FIXED_CODES[data[position]])
        for at in range(position, position + step):
            key = data[at:at + 3]
            if len(key) == 3:
                if key not in chains:
                    chains[key] = deque(maxlen=64)
                chains[key].append(at)
        position += step
    emit(*_FIXED_CODES[256])  # End-of-block; pad the last byte with zero bits.
    if available:
        output.append(accumulator)
    a, b = 1, 0
    for value in data:
        a = (a + value) % 65521
        b = (b + a) % 65521
    output.extend(((b << 16) | a).to_bytes(4, 'big'))
    return bytes(output)


def pack_records(documents):
    """NSP1: two unchanged LGL1 files, independently zlib-compressed in 8 KiB blocks.

    The absolute offset table permits constant-time jumps without decoding preceding text.
    FNV-1a covers the entire container except its own word; each block also has zlib's Adler32.
    This is a RomFS asset, never an ELF constant or a game-derived input.
    """
    if len(documents) != 2:
        raise ValueError('Legal pack requires exactly two documents')
    blocks = []
    for data in documents:
        if data[:4] != b'LGL1' or len(data) < 8:
            raise ValueError('Legal pack requires LGL1 records')
        blocks.extend(_zlib_fixed(data[i:i + 8192]) for i in range(0, len(data), 8192))
    offset = 24 + 4 * (len(blocks) + 1)
    offsets = [offset]
    for block in blocks:
        offset += len(block)
        offsets.append(offset)
    if offset > 262144:
        raise ValueError('Legal pack exceeds the bounded reader format')
    result = bytearray(struct.pack('<4s5I', b'NSP1', offset, 8192,
                                   len(documents[0]), len(documents[1]), 0))
    result.extend(struct.pack('<%dI' % len(offsets), *offsets))
    result.extend(b''.join(blocks))
    checksum = 2166136261
    for value in result[:20] + result[24:]:
        checksum = ((checksum ^ value) * 16777619) & 0xffffffff
    struct.pack_into('<I', result, 20, checksum)
    return bytes(result)


def prepare(root, output, notices=None):
    contract = json.loads((root / 'docs/release/build-contract.json').read_text(encoding='utf-8'))
    output.mkdir(parents=True, exist_ok=True)
    documents = []
    for name in ('LICENSE', 'THIRD_PARTY_NOTICES'):
        source = notices if name == 'THIRD_PARTY_NOTICES' and notices is not None else root / name
        data = source.read_bytes()
        if b'\r' in data:
            raise ValueError(f'{source}: carriage return in raw RomFS legal input; restore LF-only line endings')
        pages = paginate(data, contract['legal_columns'], contract['legal_lines'])
        write_changed(output / name, data)
        write_changed(output / (name + '.pages'), pages)
        documents.append(pages)
    write_changed(output / 'READER.lgrz', pack_records(documents))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--notices', type=Path, help='selected game\'s full notice bundle')
    args = parser.parse_args()
    prepare(args.root, args.output, args.notices)
