"""Retain the DR Old early-retry wrapper's original 268-byte late slot.

Only a marked replacement body is padded; growth fails assembly. Production
and diagnostic layout checks must still prove all unrelated slots unchanged.
"""
import re
import hashlib
import struct
import subprocess

NAME = '__wrap_CtrTextures_beginFrame'
TAIL = 'oomport_tail_FvgPalette_earlyRetry'
SLOT = 268
BIN = 'C:/devkitPro/devkitARM/bin/arm-none-eabi-'


def loaded_bytes(path, address, size):
    with path.open('rb') as handle:
        header = handle.read(52)
        offset = struct.unpack_from('<I', header, 28)[0]
        width, count = struct.unpack_from('<HH', header, 42)
        for index in range(count):
            handle.seek(offset + index * width)
            kind, at, start, _, length, _, _, _ = struct.unpack('<8I', handle.read(32))
            if kind == 1 and start <= address and address + size <= start + length:
                handle.seek(at + address - start)
                value = handle.read(size)
                assert len(value) == size
                return value
    raise AssertionError(('missing loaded slot', path, hex(address), size))


def audit_compiler_locals(base, candidate):
    """Allow GCC numeric switch-table renames with identical slots and bytes.

    This is the compiler-local rename exception in the layout rules;
    no moved table or changed functional contents qualifies.
    """
    def tables(path):
        text = subprocess.run([BIN + 'nm.exe', '-n', '-S', '--defined-only', str(path)],
                              stdin=subprocess.DEVNULL, capture_output=True,
                              text=True, check=True, timeout=30).stdout
        result = {}
        for line in text.splitlines():
            fields = line.split()
            if len(fields) == 4 and re.fullmatch(r'CSWTCH\.\d+', fields[3]):
                address, size, kind, name = fields
                result.setdefault(name, []).append((kind, int(address, 16), int(size, 16)))
        return result
    old, new = tables(base), tables(candidate)
    # Integration changes GCC's numeric suffixes. Match only unique identical
    # slots and bytes; never allow a moved table or changed contents.
    removed, added = old.keys() - new.keys(), new.keys() - old.keys()
    pairs = []
    for before in sorted(removed):
        matches = [after for after in added if old[before] == new[after]]
        assert len(matches) == 1, ('compiler-local rename is not unique', before, matches)
        pairs.append((before, matches[0]))
    assert {b for _, b in pairs} == added
    assert all(old[name] == new[name] for name in old.keys() & new.keys())
    receipts = []
    for before, after in pairs:
        assert len(old[before]) == 1 and old[before] == new[after] and old[before][0][0] == 'r'
        _, address, size = old[before][0]
        a, b = loaded_bytes(base, address, size), loaded_bytes(candidate, address, size)
        assert a == b
        receipts.append(dict(before=before, after=after, address=hex(address), bytes=size,
                             bytes_identical=True, sha256=hashlib.sha256(a).hexdigest()))
    return dict(passed=True, class_name='compiler-local rename', renames=receipts,
                allow=sorted(name for pair in pairs for name in pair))


def retain(assembly):
    marker = '@ flowervramg-early-retry'
    if marker not in assembly:
        return assembly
    name = NAME
    pattern = r'(?ms)^' + name + r':\n.*?^(\s*\.size\s+' + name + r',\s*\.-' + name + r'\s*)$'
    matches = list(re.finditer(pattern, assembly))
    assert len(matches) == 1 and assembly.count(marker) == 1, 'unknown early-retry compiler shape'
    match = matches[0]
    assert marker in match[0]
    # The fence is after the complete emitted body, including its literal pool.
    # No compiler-generated branch can target the newly appended zero padding.
    end = match.start(1)
    return assembly[:end] + '\n' + TAIL + ':\n\t.org ' + name + '+268\n' + assembly[end:]


def audit_elf(base, candidate):
    """Prove the body occupies its old slot and cannot enter the zero tail.

    This checks direct control flow and literal/address references; it does
    not claim to resolve arbitrary computed indirect targets. No source stores
    or computes this wrapper's tail address.
    """
    def command(tool, *args):
        return subprocess.run([BIN + tool + '.exe', *map(str, args)],
                              stdin=subprocess.DEVNULL, capture_output=True,
                              text=True, check=True, timeout=30).stdout

    def symbols(path):
        rows = {}
        for line in command('nm', '-n', '-S', '--defined-only', path).splitlines():
            fields = line.split()
            if len(fields) == 4:
                address, size, kind, name = fields
                if kind.lower() == 't':
                    rows[name] = (int(address, 16), int(size, 16))
            elif len(fields) == 3 and fields[2] == TAIL:
                rows[TAIL] = (int(fields[0], 16), 0)
        return rows

    old, new = symbols(base), symbols(candidate)
    assert old[NAME] == new[NAME] and new[NAME][1] == SLOT
    start, size = new[NAME]
    tail = new[TAIL][0]
    end = start + size
    assert start < tail <= end and tail % 4 == 0
    following = min((a, n, s) for n, (a, s) in old.items() if a >= end)
    assert new[following[1]] == (following[0], following[2])
    with candidate.open('rb') as handle:
        header = handle.read(52)
        offset = struct.unpack_from('<I', header, 28)[0]
        width, count = struct.unpack_from('<HH', header, 42)
        found = False
        for index in range(count):
            handle.seek(offset + index * width)
            kind, at, address, _, length, _, _, _ = struct.unpack('<8I', handle.read(32))
            if kind == 1 and address <= tail <= end <= address + length:
                handle.seek(at + tail - address)
                assert handle.read(end - tail) == bytes(end - tail)
                found = True
                break
        assert found
    instructions, targets, literal_references = [], [], []
    branch = re.compile(r'b(?:l|lx|x)?(?:eq|ne|cs|cc|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?$')
    for line in command('objdump', '-d', candidate).splitlines():
        match = re.match(r'^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+([.a-z][a-z0-9.]*)\s*(.*)$', line)
        if not match:
            continue
        address, encoded, mnemonic, operands = match.groups()
        address = int(address, 16)
        target = re.match(r'([0-9a-f]+)\s+<', operands)
        if target and branch.fullmatch(mnemonic) and tail <= int(target[1], 16) < end:
            targets.append(hex(address))
        # -d honours mapping symbols: literal pool words are data, not code.
        if mnemonic == '.word' and tail <= int(encoded, 16) < end:
            literal_references.append(hex(address))
        for value in re.findall(r'\b([0-9a-f]+)\s+<', operands):
            if not branch.fullmatch(mnemonic) and tail <= int(value, 16) < end:
                literal_references.append(hex(address))
        if start <= address < tail and not mnemonic.startswith('.'):
            instructions.append((address, mnemonic, operands))
    assert not targets and not literal_references, (targets, literal_references)
    returns = [a for a, m, o in instructions if
               (m.startswith(('pop', 'ldm')) and re.search(r'\bpc\b', o)) or
               (m.startswith('bx') and o.strip() == 'lr') or
               (m == 'mov' and o.strip() == 'pc, lr')]
    assert returns and max(returns) < tail
    last = instructions[-1]
    assert last[0] in returns or last[1] in ('b', 'bx'), last
    return dict(passed=True, original_start=hex(start), slot_B=size,
                tail_start=hex(tail), zero_tail_B=end - tail,
                next_symbol=following[1], next_address=hex(following[0]),
                direct_branch_targets_in_tail=targets, literal_references_in_tail=literal_references,
                final_instruction=list(last), indirect_target_proof=False)
