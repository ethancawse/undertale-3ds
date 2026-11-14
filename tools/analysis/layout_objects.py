"""Qualified ELF OBJECT placement checks shared by the lab layout gates.

Input ownership comes from the link map; local identity is independently bound
to the ELF STT_FILE block. Waivers pin the ELF pair and verify a destination.
"""
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
import hashlib
import json
import posixpath
import re
import struct


@dataclass(frozen=True)
class Object:
    owner: str
    section: str
    occurrence: int
    name: str
    address: int
    size: int
    output: str = ''
    file_origin: str = ''
    local: bool = False
    digest: str = ''

    @property
    def key(self):
        return self.owner, self.section, self.occurrence, self.name

    @property
    def label(self):
        return f'{self.owner}:{self.section}#{self.occurrence}:{self.name}'


def owner_name(value):
    value = value.replace('\\', '/')
    if re.search(r'\.a\([^()]+\)$', value):
        archive, member = value.rsplit('(', 1)
        return owner_name(archive) + '(' + owner_name(member[:-1]) + ')'
    value = posixpath.normpath(value)
    # Toolchain installation and checkout roots are not input identities.
    for anchor in ('arm-none-eabi/lib/', 'lib/gcc/arm-none-eabi/', 'libctru/', 'libcitro3d/', 'portlibs/'):
        if anchor in value:
            return value[value.index(anchor):]
    match = re.search(r'(?:^|/)(build/[^/]+/.*\.a)$', value)
    if match:
        return match[1]
    if 'CMakeFiles/' in value:
        value = value[value.index('CMakeFiles/'):]
        # CMake encodes absolute source paths below <target>.dir. Keep the
        # repository-relative source identity, not the worktree's checkout prefix.
        match = re.match(r'^(CMakeFiles/[^/]+\.dir/)(.*)$', value)
        if match:
            relative = re.search(r'(?:^|/)((?:engine|platform/3ds/source|build/aot-[^/]+)/.*)$', match[2])
            if relative:
                return match[1] + relative[1]
        return value
    return value.removeprefix('./')


class Inventory(list):
    """Objects plus provenance for logs and FUNC/OBJECT name collisions."""
    def __init__(self, elf, map_path, blob):
        super().__init__()
        self.functions = set()
        self.elf_hash = hashlib.sha256(blob).hexdigest()
        self.map_path = Path(map_path).resolve()
        self.map_hash = hashlib.sha256(self.map_path.read_bytes()).hexdigest()


def inventory_lines(base, cand):
    lines = [f'objects: base {len(base)}, cand {len(cand)}; '
             f'locals {sum(o.local for o in base)} -> {sum(o.local for o in cand)}; '
             f'ambiguous FILE groups {len(ambiguous_files(base))} -> {len(ambiguous_files(cand))}']
    for side, values in (('base', base), ('candidate', cand)):
        if isinstance(values, Inventory):
            lines.append(f'OBJECT provenance {side}: ELF SHA256 {values.elf_hash}; '
                         f'map {values.map_path}; map SHA256 {values.map_hash}')
    return lines


def ambiguous_files(values):
    """Same local name/size and indistinguishable FILE provenance across inputs.

    Preserve full FILE paths when present. A basename-only collision cannot be
    resolved using a potentially stale map, even when its extents look correct.
    """
    groups = defaultdict(set)
    for obj in values:
        if obj.local:
            groups[obj.name, obj.size, obj.file_origin].add(obj.key)
    return {key: identities for key, identities in groups.items() if len(identities) > 1}


def cswtch_renames(base, cand, base_names, cand_names):
    """Bijective retired/new GCC table names with identical storage and TU.

    Digests cover every OBJECT byte (including NOBITS zeros), as read_objects
    computes them from each ELF. Names include unsized/non-OBJECT symbols too;
    an alias must not make a still-present name look retired. This deliberately
    excludes overlapping rename chains. No generic name allowance is produced.
    """
    groups = (defaultdict(list), defaultdict(list))
    for side, values, opposite_names in ((0, base, cand_names), (1, cand, base_names)):
        ambiguous = {key for keys in ambiguous_files(values).values() for key in keys}
        for obj in values:
            if (re.fullmatch(r'CSWTCH\.[0-9]+', obj.name) and obj.name not in opposite_names
                    and obj.digest and obj.file_origin and obj.key not in ambiguous
                    and obj.name not in getattr(values, 'functions', set())):
                slot = (obj.owner, obj.section, obj.occurrence, obj.output, obj.file_origin,
                        obj.local, obj.address, obj.size, obj.digest)
                groups[side][slot].append(obj)
    pairs = []
    for slot, before in sorted(groups[0].items()):
        after = groups[1].get(slot, [])
        if len(before) == len(after) == 1:
            pairs.append((before[0], after[0]))
    return pairs


def map_inputs(path):
    """All linked input extents, retaining full object paths and repeated sections."""
    rows, counts = [], Counter()
    output, pending = None, None
    with Path(path).open(encoding='utf-8', errors='replace') as stream:
        for line in stream:
            if line and not line[0].isspace():
                match = re.match(r'^(\.\S+)(?:\s+0x[0-9a-f]+\s+0x[0-9a-f]+)?\s*$', line)
                output = match[1] if match else None
                pending = None
                continue
            if not output:
                continue
            match = re.match(r'^ (\.\S+|COMMON)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.+?)\s*$', line)
            if match:
                section, address, size, owner = match.groups()
            else:
                if re.match(r'^ (?:\.\S+|COMMON)\s*$', line):
                    pending = line.strip()
                    continue
                match = re.match(r'^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.+?)\s*$', line)
                if not pending or not match:
                    pending = None
                    continue
                section, (address, size, owner) = pending, match.groups()
            pending = None
            if not re.search(r'\.(?:o|obj)(?:\))?$', owner):
                continue
            owner = owner_name(owner)
            key = output, owner, section
            occurrence = counts[key]
            counts[key] += 1
            rows.append((output, int(address, 16), int(size, 16), owner, section, occurrence))
    return rows


def discover_map(elf, explicit=None):
    if explicit:
        return Path(explicit)
    elf = Path(elf)
    for path in (elf.with_suffix('.map'), Path(str(elf) + '.map'), elf.parent / 'release-link.map'):
        if path.is_file():
            return path
    raise ValueError(f'OBJECT ownership needs a link map for {elf}; supply --base-map/--map')


def read_objects(elf, map_path=None):
    """Read allocated, non-TLS STT_OBJECT entries, including zero-size/local ones."""
    blob = Path(elf).read_bytes()
    if blob[:7] != b'\x7fELF\x01\x01\x01' or len(blob) < 52:
        raise ValueError(f'not a little-endian ELF32: {elf}')
    offset = struct.unpack_from('<I', blob, 32)[0]
    stride, count, names_index = struct.unpack_from('<HHH', blob, 46)
    if stride != 40 or offset + count * stride > len(blob):
        raise ValueError(f'invalid ELF section table: {elf}')
    sections = [struct.unpack_from('<10I', blob, offset + i * stride) for i in range(count)]
    if not any(section[1] == 2 for section in sections):
        raise ValueError(f'OBJECT ownership needs an ELF symbol table: {elf}')
    def contents(row):
        return blob[row[4]:row[4] + row[5]]
    def string(table, at):
        return table[at:].split(b'\0', 1)[0].decode('utf-8', errors='strict')
    names = contents(sections[names_index])
    map_path = discover_map(elf, map_path)
    result = Inventory(elf, map_path, blob)
    inputs = map_inputs(map_path)
    by_output = defaultdict(list)
    for row in inputs:
        by_output[row[0]].append(row)
    for table in sections:
        if table[1] != 2:  # SHT_SYMTAB, not a second copy from .dynsym
            continue
        if table[9] != 16 or table[5] % 16:
            raise ValueError(f'invalid ELF symbol table: {elf}')
        strings = contents(sections[table[6]])
        file_origin = ''
        for at in range(table[4], table[4] + table[5], 16):
            name, address, size, info, _other, index = struct.unpack_from('<IIIBBH', blob, at)
            if info & 15 == 4:  # STT_FILE: independent of map-derived input ownership.
                file_origin = string(strings, name)
                continue
            if info & 15 == 2 and 0 < index < len(sections):
                result.functions.add(string(strings, name))
            if info & 15 != 1 or not 0 < index < len(sections):
                continue
            section = sections[index]
            if not section[2] & 2 or section[2] & 0x400:
                continue
            name = string(strings, name)
            local = info >> 4 == 0
            output = string(names, section[0])
            # GNU ld synthesizes this local OBJECT before any input FILE block.
            origin = file_origin
            if local and not origin and name == '_GLOBAL_OFFSET_TABLE_' and output in ('.got', '.got.plt'):
                origin = '<linker GOT>'
            if local and not origin:
                raise ValueError(f'local OBJECT {name} has no ELF STT_FILE block: {elf}')
            # Half-open intervals prevent the preceding section from owning a
            # zero-size symbol at the start of the following input section.
            owners = [row for row in by_output[output]
                      if row[1] <= address < row[1] + row[2]
                      and address + size <= row[1] + row[2]]
            if not owners and not size:
                owners = [row for row in by_output[output] if row[1] == address and row[2] == 0]
            identities = {(row[3], row[4], row[5]) for row in owners}
            if len(identities) != 1:
                raise ValueError(f'OBJECT ownership ambiguous or missing: {name} {address:#x}/{size} B '
                                 f'in {output} ({len(identities)} owners); map {map_path or elf}')
            owner, input_section, occurrence = identities.pop()
            data = bytes(size) if section[1] == 8 else blob[
                section[4] + address - section[3]:section[4] + address - section[3] + size]
            result.append(Object(owner, input_section, occurrence, name, address, size,
                                 output, origin if local else '', local,
                                 hashlib.sha256(data).hexdigest()))
    return result


def parse_waivers(rows):
    if not isinstance(rows, list):
        raise ValueError('object_waivers must be a list')
    result = []
    for row in rows:
        if not isinstance(row, dict) or set(row) - {
                'object', 'section', 'symbol', 'occurrence', 'renamed_to', 'destination_address'} \
                or not all(isinstance(row.get(key), str) and row[key]
                           for key in ('object', 'section', 'symbol', 'renamed_to')):
            raise ValueError('OBJECT waiver must name object, section, symbol and renamed_to; '
                             'destination must preserve size, bytes and FILE identity')
        if any('*' in row[key] or '?' in row[key] for key in ('object', 'section', 'symbol', 'renamed_to')):
            raise ValueError('OBJECT waivers must be exact, without wildcards')
        if 'occurrence' in row and (type(row['occurrence']) is not int or row['occurrence'] < 0):
            raise ValueError('OBJECT waiver occurrence must be a non-negative integer')
        if 'destination_address' in row and (type(row['destination_address']) is not int
                                             or row['destination_address'] < 0):
            raise ValueError('OBJECT waiver destination_address must be a non-negative integer')
        normalized = dict(row, object=owner_name(row['object']))
        if normalized in result:
            raise ValueError('duplicate OBJECT waiver')
        result.append(normalized)
    return result


def waiver_file(path, game=None, base=None, cand=None):
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    if game:
        if not isinstance(data, dict) or game not in data:
            raise ValueError(f'OBJECT waiver manifest has no entry for {game}')
        data = data[game]
    waivers = parse_waivers(data.get('object_waivers', []) if isinstance(data, dict) else data)
    if waivers:
        for label, elf in (('base', base), ('candidate', cand)):
            expected = data.get(label + '_sha256') if isinstance(data, dict) else None
            if not isinstance(expected, str) or not re.fullmatch('[0-9a-f]{64}', expected) or elf is None:
                raise ValueError(f'OBJECT waivers require {label}_sha256 and the exact ELF pair')
            if hashlib.sha256(Path(elf).read_bytes()).hexdigest() != expected:
                raise ValueError(f'OBJECT waiver {label}_sha256 mismatch: regenerate after auditing this pair')
    return waivers


def compare_objects(base, cand, waivers=(), expect_shift=False, allow_inventory=False, *, aot_objects=None):
    """Return problems, accepted labels, and exact base identity keys waived.

    No generic name allowlist enters this function. A basename is accepted only
    when it selects one owner/section/symbol identity across both inputs.
    The AOT selection verifier may supply a predicate over ALL same-name OBJECT
    definitions on both sides. It proves each changed definition belongs to an
    AOT input in its own map; no CLI name allowance enables that exception.
    """
    old, new = defaultdict(list), defaultdict(list)
    for obj in base:
        old[obj.key].append(obj)
    for obj in cand:
        new[obj.key].append(obj)
    problems, accepted, allowed = [], [], set()
    for side, values in (('base', base), ('candidate', cand)):
        for (name, size, origin), identities in sorted(ambiguous_files(values).items()):
            problems.append(f'OBJECT ambiguous FILE group {side}: {name}/{size} B FILE {origin!r}; '
                            f'inputs {", ".join(sorted(f"{k[0]}:{k[1]}#{k[2]}" for k in identities))}; '
                            'rename one static or one source file')
    if not allow_inventory and (not base or not cand or abs(len(base) - len(cand)) > len(base) * .05):
        problems.append(f'OBJECT inventory changed unexpectedly: {len(base)} -> {len(cand)}; '
                        'audit before using --allow-inventory (placements remain checked)')
    if not allow_inventory and any(isinstance(xs, Inventory) and not any(o.local for o in xs)
                                   for xs in (base, cand)):
        problems.append('OBJECT local inventory is empty: locals-stripped ELF is not a qualification input')
    # A reserve expansion translates complete output sections by whole pages.
    # One outlier, missing/added identity, size or FILE change rejects the class.
    shifted = set()
    if expect_shift:
        for output in sorted({o.output for o in base if o.output}):
            keys = {o.key for o in base if o.output == output}
            dest_keys = {o.key for o in cand if o.output == output}
            pairs = [(old[k][0], new[k][0]) for k in keys
                     if len(old[k]) == len(new.get(k, [])) == 1]
            if keys != dest_keys or len(pairs) != len(keys):
                continue
            deltas = {b.address - a.address for a, b in pairs}
            if len(deltas) != 1:
                continue
            delta = next(iter(deltas))
            if not delta or delta % 4096 or any(
                    (a.size, a.file_origin, a.local) != (b.size, b.file_origin, b.local) for a, b in pairs):
                continue
            shifted.update(keys)
            accepted.append(f'OBJECT SHIFT {output}: {len(keys)} objects {delta:+d} B '
                            f'({delta // 4096:+d} pages); 0 missing, 0 resized; shiftproof still required')
    universe = {obj.key: obj for obj in (*base, *cand)}
    for waiver in parse_waivers(list(waivers)):
        matches = [key for key, obj in universe.items()
                   if (obj.owner == waiver['object'] or obj.owner.rsplit('/', 1)[-1] == waiver['object'])
                   and obj.section == waiver['section'] and obj.name == waiver['symbol']
                   and ('occurrence' not in waiver or obj.occurrence == waiver['occurrence'])]
        if len(matches) != 1 or matches[0] not in old:
            problems.append(f'OBJECT waiver must select one pre-existing identity: {waiver}; matches {len(matches)}')
            continue
        key = matches[0]
        source = old[key]
        if len(source) != 1:
            problems.append(f'OBJECT waiver source is ambiguous: {waiver}')
            continue
        obj = source[0]
        if key in shifted:
            problems.append(f'OBJECT waiver unused (uniform-shift class): {obj.label}')
            continue
        dest = [o for o in cand if (o.owner, o.section, o.occurrence, o.name) == (
            obj.owner, obj.section, obj.occurrence, waiver['renamed_to'])]
        if len(dest) != 1:
            problems.append(f'OBJECT waiver destination must select one identity: {waiver}')
            continue
        target = dest[0]
        if ((obj.size, obj.file_origin, obj.local, obj.digest) !=
                (target.size, target.file_origin, target.local, target.digest)
                or not obj.digest
                or target.address != waiver.get('destination_address', obj.address)):
            problems.append(f'OBJECT waiver destination failed address/size/bytes/FILE proof: {obj.label}')
            continue
        if key == target.key and obj.address == target.address:
            problems.append(f'OBJECT waiver unused: {obj.label}')
            continue
        allowed.add(key)
        accepted.append(f'OBJECT ALLOWED {obj.label}: {obj.address:#x}/{obj.size} B -> '
                        f'{target.label} {target.address:#x}/{target.size} B; same bytes and FILE')
    waiver_count = len(allowed)
    aot_allowed = set()
    if aot_objects is not None:
        by_name = defaultdict(lambda: ([], []))
        for side, values in enumerate((base, cand)):
            for obj in values:
                by_name[obj.name][side].append(obj)
        for before, after in by_name.values():
            if aot_objects(before, after):
                aot_allowed.update(obj.key for obj in before)
    for key, values in sorted(old.items()):
        candidate = new.get(key, [])
        obj = values[0]
        if len(values) != 1 or len(candidate) > 1:
            problems.append(f'OBJECT ambiguous duplicate {obj.label}: {len(values)} -> {len(candidate)} definitions')
            continue
        if key in allowed or key in shifted:
            continue
        if candidate and (obj.address, obj.size, obj.file_origin, obj.local, obj.output) == (
                candidate[0].address, candidate[0].size, candidate[0].file_origin, candidate[0].local,
                candidate[0].output):
            continue
        if key in aot_allowed:
            allowed.add(key)
            accepted.append(f'OBJECT AOT {obj.label}: every changed definition inside its own AOT input')
            continue
        before = f'{obj.address:#x}/{obj.size} B'
        after = f'{candidate[0].address:#x}/{candidate[0].size} B' if candidate else 'missing'
        if key not in allowed:
            changes = []
            if not candidate:
                changes.append('missing')
            else:
                if obj.address != candidate[0].address:
                    changes.append(f'moved {candidate[0].address - obj.address:+d} B')
                if obj.size != candidate[0].size:
                    changes.append('resized')
                if (obj.file_origin, obj.local) != (candidate[0].file_origin, candidate[0].local):
                    changes.append(f'FILE identity {obj.file_origin!r} -> {candidate[0].file_origin!r}')
                if obj.output != candidate[0].output:
                    changes.append('output section changed')
            problems.append(f'OBJECT {", ".join(changes)} {obj.label}: {before} -> {after}; '
                            'requires explicit object_waivers entry')
    if waivers:
        accepted.append(f'OBJECT waivers used: {waiver_count}/{len(waivers)}; unused is a failure')
    return problems, accepted, allowed
