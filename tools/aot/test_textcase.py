"""Tests for tools/aot/textcase.py (romfs/textcase.bin, the dialogue script's case table; source/z_textcase.c reads it).

    py -3 -I tools/aot/test_textcase.py

The synthetic bytecode names no game identifiers. The last test checks the real table against the player's UNDERTALE
data when it is installed (CTRPORT_UNDERTALE_DATA or Steam), and is skipped otherwise.
"""
import contextlib
import importlib.util
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import textcase  # noqa: E402


@contextlib.contextmanager
def aot_prepare(directory):
    """tools/aot/prepare.py loaded by path, and 'prepare' only inside the block. In one pytest process over every
    tool, tools/audio/prepare.py can already be sys.modules['prepare'] or come first on sys.path, so a bare import may
    not be this file. sys.modules and sys.path are put back afterwards."""
    missing = object()
    old, path = sys.modules.get('prepare', missing), sys.path[:]
    spec = importlib.util.spec_from_file_location('prepare', os.path.join(directory, 'prepare.py'))
    module = importlib.util.module_from_spec(spec)
    sys.modules['prepare'] = module
    try:
        spec.loader.exec_module(module)
        yield module
    finally:
        sys.path[:] = path
        if old is missing:
            sys.modules.pop('prepare', None)
        else:
            sys.modules['prepare'] = old

PUSHBLTN_V = 0xC305FFFF  # PushBltn.v self.<var>


def bt(at, target):
    return 0xB7000000 | (((target - at) // 4) & 0x7FFFFF)


def b(at, target):
    return 0xB6000000 | (((target - at) // 4) & 0x7FFFFF)


def script(keys, body=8, first=PUSHBLTN_V):
    """Bytecode: the switch value's push, one case per key, the chain's B, then each case's body (body bytes of
    Popz-like filler) and a Popz at the end; (bytes, [case address per key], address of the chain's B)."""
    n = len(keys)
    chain_at = 8
    end_b = chain_at + 16 * n
    bodies = end_b + 4
    targets = [bodies + i * body for i in range(n)]
    tail = bodies + n * body
    words = [first, 0xA0000007]
    for i, k in enumerate(keys):
        at = chain_at + 16 * i
        words += [textcase.DUP_V0, textcase.PUSHI_E | (k & 0xFFFF), textcase.CMP_IV_EQ, bt(at + 12, targets[i])]
    words.append(b(end_b, tail))
    words += [0x9E050000] * (n * body // 4)
    words.append(0x9E050000)
    return struct.pack('<%dI' % len(words), *words), targets, end_b


class FakeDw:
    def __init__(self, blob, name='x', index=3):
        self.data = bytearray(b'\0' * 16 + blob)
        self.entry = {'index': index, 'name': name, 'length': len(blob), 'abs': 16, 'offset': 0}
        self.code = [None, self.entry]
        self.vars = [{'name': 'v%d' % i} for i in range(7)] + [{'name': 'argument0'}]


def entries(blob):
    head = struct.unpack_from('<5I', blob, 0)
    rows = [struct.unpack_from('<hH', blob, 20 + 4 * i) for i in range(head[4])]
    return head, rows


class TestTable(unittest.TestCase):
    def test_sorted_with_addresses(self):
        keys = [5, -3, 200, 7] + list(range(1000, 1020))
        code, targets, end_b = script(keys)
        dw = FakeDw(code)
        blob = textcase.table(dw, dw.entry)
        head, rows = entries(blob)
        self.assertEqual(head[0], 3)
        self.assertEqual(head[1], len(code))
        self.assertEqual(head[2], end_b + 4)
        self.assertEqual(head[3], textcase.fnv1a32(code[:end_b + 4]))
        self.assertEqual(len(blob), 20 + 4 * head[4])
        self.assertEqual([k for k, _ in rows], sorted(keys))
        want = dict(zip(keys, targets))
        self.assertEqual({k: a * 4 for k, a in rows}, want)

    def test_repeated_constant_keeps_first(self):
        keys = list(range(20)) + [4]
        code, targets, _ = script(keys)
        dw = FakeDw(code)
        _, rows = entries(textcase.table(dw, dw.entry))
        self.assertEqual(len(rows), 20)
        self.assertEqual(dict(rows)[4] * 4, targets[4])

    def test_shapes_refused(self):
        code, _, _ = script(list(range(textcase.MIN_CASES - 1)))
        self.assertIsInstance(textcase.table(FakeDw(code), FakeDw(code).entry), str)
        code, _, _ = script(list(range(20)), first=0xC005FFFF)
        dw = FakeDw(code)
        self.assertIsInstance(textcase.table(dw, dw.entry), str)
        # A switch on another variable (the runtime reads argument0 only).
        code, _, _ = script(list(range(20)))
        dw = FakeDw(code)
        dw.vars[7] = {'name': 'argument1'}
        self.assertIsInstance(textcase.table(dw, dw.entry), str)
        # A chain that doesn't end in B.
        code, _, end_b = script(list(range(20)))
        bad = code[:end_b] + struct.pack('<I', 0x9E050000) + code[end_b + 4:]
        dw = FakeDw(bad)
        self.assertIsInstance(textcase.table(dw, dw.entry), str)
        # A case branching back into the chain.
        code, _, _ = script(list(range(20)))
        words = list(struct.unpack('<%dI' % (len(code) // 4), code))
        words[2 + 3] = bt(8 + 12, 8)
        dw = FakeDw(struct.pack('<%dI' % len(words), *words))
        self.assertIsInstance(textcase.table(dw, dw.entry), str)

    def test_find_by_hash(self):
        code, _, _ = script(list(range(20)))
        dw = FakeDw(code, name='some name')
        self.assertEqual(textcase.find(dw), 'script not found')
        old = textcase.SCRIPT_KEY
        try:
            textcase.SCRIPT_KEY = textcase.name_hash('some name')
            self.assertIs(textcase.find(dw), dw.entry)
        finally:
            textcase.SCRIPT_KEY = old

    def test_write_removes_empty(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(textcase.write(d, b'abc'))
            self.assertFalse(textcase.write(d, b'abc'))
            self.assertTrue(textcase.write(d, b''))
            self.assertFalse(os.path.exists(os.path.join(d, 'textcase.bin')))


class TestRealData(unittest.TestCase):
    def test_undertale(self):
        with aot_prepare(HERE) as prepare:
            self.undertale(prepare)

    def undertale(self, prepare):
        import corpus
        found = prepare.find_data('undertale', None)
        parts = [p for _, p, _ in corpus.parts('undertale', found)] if found else []
        if not parts:
            self.skipTest('no UNDERTALE data')
        blob, line = textcase.from_data_win(parts[0])
        self.assertTrue(blob, line)
        head, rows = entries(blob)
        self.assertGreaterEqual(head[4], textcase.MIN_CASES)
        self.assertEqual([k for k, _ in rows], sorted(set(k for k, _ in rows)))


if __name__ == '__main__':
    unittest.main()
