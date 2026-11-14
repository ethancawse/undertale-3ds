"""Explicit UNDERTALE AOT placements; no game code is stored here.

Append a new id for each round. Never grow an earlier round. Sizes describe
retained original inputs, not spare capacity; linked proofs must check them.
"""
import json
from pathlib import Path
import re

MANIFEST = Path(__file__).with_suffix('.json')


def routes():
    data = json.loads(MANIFEST.read_text(encoding='utf-8'))
    if data['version'] != 1:
        raise ValueError('unsupported AAC AOT manifest')
    entries, ids = set(), set()
    for row in data['functions']:
        if (not re.fullmatch(r'gml_\w+', row['entry']) or not re.fullmatch(r'[A-Za-z]\w*', row['id'])
                or row['entry'] in entries or row['id'] in ids):
            raise ValueError('invalid or duplicate AAC AOT route')
        for model in ('o3ds', 'n3ds'):
            if (type(row['size'][model]) is not int or row['size'][model] <= 0
                    or row['size'][model] % 4 or type(row['phase'][model]) is not int
                    or row['phase'][model] not in range(0, 32, 4)):
                raise ValueError('invalid AAC AOT size/phase')
        entries.add(row['entry'])
        ids.add(row['id'])
    return data['functions']


def place(text, wad, entry, number, target):
    if wad != 16 or target not in ('o3ds', 'n3ds'):
        return text
    found = [row for row in routes() if row['entry'] == entry]
    if not found:
        return text
    row = found[0]
    head = f'RValue aotf_{number}(VMContext* ctx) {{'
    if text.count(head) != 1:
        raise ValueError('AAC AOT route requires one ordinary generated definition')
    # One physical line: later functions retain every __LINE__ constant.
    marker = f'__{row["id"]}_present'
    prefix = (f'/* AACSLOT {row["id"]} {row["size"][target]} */ '
              f'__asm__(".global {marker}\\n.set {marker},1"); '
              f'__attribute__((section(".aacout.text.{row["id"]}"))) ')
    return text.replace(head, prefix + head)
