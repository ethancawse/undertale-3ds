"""Compare a candidate 3DS ELF's layout with its base: warm text, .rodata, .data and .bss moves, and the reserve left.

    py -3 -I tools/analysis/check_layout.py <base.elf> <candidate.elf> [--full] [--allow NAME ...] [--expect-shift]

Every sized symbol (arm-none-eabi-nm -S) is matched by name and nm type, repeated local names in order. A symbol
that exists in the base must keep its address and size in the candidate; a symbol only the candidate has must lie in
a late section (.late.text, .late.rodata, .late.data, .late.bss: platform/3ds/late_reserve.ld), or entirely
inside an AAC slot's newly occupied suffix. AAC slot extents must stay fixed and occupancy cannot shrink; this
rule accepts additions only and never waives a pre-existing symbol's placement or identity. Every
allocated section of the base other than those must keep its address and size too, the frozen late sections
(.fatalflush, .vidfix, .heapfrag, .heapfrag_rodata) included. --allow NAME accepts any change to that symbol (a
function deliberately moved to late code, say). Pre-existing ELF OBJECT symbols have an independent qualified
check: generic --allow never waives them. --expect-shift verifies complete output sections moving by one page
multiple, with equal sizes, unchanged FILE identities and no added/missing objects. All sections in one PT_LOAD
must follow that segment's single whole-page delta. It still needs shiftproof.
Supply --object-waivers JSON (optionally --game) with base_sha256, candidate_sha256 and object_waivers entries
{"object": "file.o", "section": ".bss.input", "symbol": "name", "renamed_to": "name.1"}; repeated input sections
can also require "occurrence". The destination must keep size/bytes/FILE and address (or an explicit integer
destination_address). Unused waivers fail. A link map is required for each ELF: --base-map/--map or an adjacent .map,
<elf>.map or release-link.map. Same-name statics are matched by object/input section, not address order. Full FILE
paths distinguish local identities; indistinguishable same-name/size FILE groups fail closed, even with inventory
override, because a stale map could otherwise hide a swap.
The AOT verifier additionally opts into allow_cswtch_renames: retired/new
CSWTCH.N OBJECT names with bijective, identical storage and object/TU identity
are compared under the old name in private copies and reported explicitly as
WAIVED-CSWTCH-RENAME. Other callers retain the default strict comparison.
--expect-shift is for a deliberate one-time move of the data
segments, such as adding the reserves: text must still not move, and the rodata, data and bss moves are summarised
as delta histograms instead of failing. TLS remains independent: --expect-shift alone is not the complete gate.
Without it, the tool is the gate for new late code: 0 failures means
nothing that existed moved.

The report ends with the reserve left in the candidate (from late_reserve.ld's __late_*_start/end/limit symbols), or
each segment's page padding when the candidate has no reserve. Both inputs must be built alike: same game, model,
flags, AOT set and clean/dirty state. Exit status 0 when there are no failures.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass, replace
import copy
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from layout_objects import read_objects, compare_objects, waiver_file, inventory_lines, cswtch_renames  # noqa: E402

DEVKITARM_BIN = Path('C:/devkitPro/devkitARM/bin')
FROZEN = ('.fatalflush', '.vidfix', '.heapfrag', '.heapfrag_rodata')
LANE_LATE = ('.late.text', '.late.rodata', '.late.data', '.late.bss')
RESERVES = ('text', 'rodata', 'data')
AAC_CAPACITIES = {'text': 43396, 'rodata': 23532}
# The capacities a slot may have: UNDERTALE New keeps 43,396 B of AAC text, UNDERTALE Old has 80,260 B since the
# Old 1.0 envelope (relready-resize.md, dv8q). A layout pair must still have the same extent on both sides.
AAC_CAPACITY_SETS = {'text': (43396, 80260), 'rodata': (23532,)}
PAGE = 0x1000


@dataclass(frozen=True)
class Section:
    name: str
    kind: str  # PROGBITS, NOBITS, ...
    addr: int
    size: int
    flags: str

    @property
    def end(self) -> int:
        return self.addr + self.size

    @property
    def region(self) -> str:
        if 'X' in self.flags:
            return 'text'
        if 'W' not in self.flags:
            return 'rodata'
        return 'bss' if self.kind == 'NOBITS' else 'data'


@dataclass
class Layout:
    sections: list[Section]
    symbols: dict[tuple[str, str], list[tuple[int, int]]]
    plain: dict[str, int]  # unsized symbols (late_reserve.ld's limits), by name
    segments: list[tuple[int, int, int]]  # (vaddr, filesz, memsz) of each PT_LOAD
    tls: list[Section] | None = None  # TLS sections (.tdata, .tbss): compared by place, not used for regions
    objects: list | None = None  # Qualified STT_OBJECT identities from ELF and link map.

    def section_at(self, addr: int, size: int = 1) -> Section | None:
        for section in self.sections:
            if section.addr <= addr and addr + max(size, 1) <= section.end and section.size:
                return section
        return None


def parse_sections(text: str, tls: bool = False) -> list[Section]:
    """Allocated sections from `readelf -SW`, skipping TLS (whose symbols are offsets, not addresses); with tls, the
    TLS ones only (argbenchr7b R7B-3.1: their place and size are compared too)."""
    sections = []
    for line in text.splitlines():
        if ']' not in line or not line.lstrip().startswith('['):
            continue
        fields = line.split(']', 1)[1].split()
        if len(fields) < 9 or fields[0] in ('Name', 'NULL'):
            continue
        name, kind, addr, _off, size, _es = fields[:6]
        rest = fields[6:]
        flags = rest[0] if len(rest) == 4 else ''
        try:
            section = Section(name, kind, int(addr, 16), int(size, 16), flags)
        except ValueError:
            continue
        if 'A' in flags and ('T' in flags) == tls:
            sections.append(section)
    return sorted(sections, key=lambda s: (s.addr, s.size))


def parse_segments(text: str) -> list[tuple[int, int, int]]:
    segments = []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 6 and fields[0] == 'LOAD':
            segments.append((int(fields[2], 16), int(fields[4], 16), int(fields[5], 16)))
    return segments


def parse_symbols(text: str) -> tuple[dict[tuple[str, str], list[tuple[int, int]]], dict[str, int]]:
    """Sized symbols from `nm -n -S --defined-only`, and unsized ones by name."""
    sized = defaultdict(list)
    plain = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 4:
            sized[(parts[3], parts[2])].append((int(parts[0], 16), int(parts[1], 16)))
        elif len(parts) == 3:
            plain[parts[2]] = int(parts[0], 16)
    return dict(sized), plain


def tool(name: str, override: str | None) -> str:
    if override:
        return override
    found = shutil.which(f'arm-none-eabi-{name}')
    return found or str(DEVKITARM_BIN / f'arm-none-eabi-{name}.exe')


def run(command: list[str]) -> str:
    return subprocess.run(command, capture_output=True, text=True, check=True, timeout=60,
                          stdin=subprocess.DEVNULL).stdout


def load(path: Path, nm: str, readelf: str, map_path: Path | None = None) -> Layout:
    # This entry point is also used by change CLIs which do not catch ValueError.
    # Exit like a CLI tool error there too, never traceback or return an empty gate.
    try:
        symbols, plain = parse_symbols(run([nm, '-n', '-S', '--defined-only', str(path)]))
        sections = run([readelf, '-SW', str(path)])
        return Layout(parse_sections(sections), symbols, plain, parse_segments(run([readelf, '-lW', str(path)])),
                      parse_sections(sections, tls=True), read_objects(path, map_path))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'LAYOUT TOOL ERROR: {error}', file=sys.stderr)
        raise SystemExit(2) from None


def is_lane_late(section: Section | None) -> bool:
    return section is not None and section.name in LANE_LATE


def is_reserve(section: Section) -> bool:
    return section.name.startswith('.late.') and section.name.endswith('.reserve')


def aac_append_regions(base: Layout, cand: Layout) -> list[tuple[int, int]]:
    """Qualified new occupancy only; absent markers confer no permission."""
    regions = []
    for kind, capacity in AAC_CAPACITIES.items():
        names = [f'__aacout_{kind}_{part}' for part in ('start', 'used_end', 'end')]
        if not any(name in layout.plain for layout in (base, cand) for name in names):
            continue
        if not all(name in layout.plain for layout in (base, cand) for name in names):
            raise ValueError(f'AAC {kind}: incomplete slot markers')
        start, used, end = (base.plain[name] for name in names)
        cstart, cused, cend = (cand.plain[name] for name in names)
        if (start, end) != (cstart, cend) or end - start not in AAC_CAPACITY_SETS[kind]:
            raise ValueError(f'AAC {kind}: slot extent changed')
        capacity = end - start
        if not start <= used <= cused <= end:
            raise ValueError(f'AAC {kind}: invalid or shrinking occupancy')
        sections = [layout.section_at(start, capacity) for layout in (base, cand)]
        if any(section is None or section.name != '.' + kind or 'W' in section.flags
               or ('X' in section.flags) != (kind == 'text') for section in sections):
            raise ValueError(f'AAC {kind}: slot is not inside its read-only output section')
        regions.append((used, cused))
    return regions


class Report:
    def __init__(self, full: bool):
        self.full = full
        self.lines: list[str] = []
        self.failures = 0

    def say(self, line: str) -> None:
        self.lines.append(line)

    def fail(self, line: str) -> None:
        self.failures += 1
        self.lines.append('FAIL ' + line)


def check_segment_shifts(base: Layout, cand: Layout, report: Report) -> None:
    """Every non-filler output section must follow its corresponding PT_LOAD."""
    if not base.segments and not cand.segments:  # synthetic, non-loaded unit fixtures
        return
    if len(base.segments) != len(cand.segments):
        report.fail('SHIFT load segment count changed')
        return
    sections = {s.name: s for s in cand.sections}
    for old in base.sections:
        new = sections.get(old.name)
        if new is None or is_reserve(old) or not old.size:
            continue  # inventory/extent checks below handle missing sections and fillers
        indices = [i for i, (addr, _, length) in enumerate(base.segments)
                   if addr <= old.addr and old.end <= addr + length]
        if len(indices) != 1:
            report.fail(f'SHIFT section {old.name}: ambiguous or missing base PT_LOAD')
            continue
        index = indices[0]
        address, _, length = cand.segments[index]
        delta = address - base.segments[index][0]
        if (delta % PAGE or new.addr - old.addr != delta
                or not (address <= new.addr and new.end <= address + length)):
            report.fail(f'SHIFT section {old.name}: address delta {new.addr - old.addr:+d} B, '
                        f'PT_LOAD {index} delta {delta:+d} B; must follow one whole-page segment delta')


def compare(base: Layout, cand: Layout, allow: frozenset[str] = frozenset(), expect_shift: bool = False,
            full: bool = False, object_waivers=(), allow_inventory=False, aot_objects=None,
            allow_cswtch_renames=False) -> Report:
    report = Report(full)
    try:
        aac_regions = aac_append_regions(base, cand)
    except ValueError as error:
        report.fail(str(error))
        aac_regions = []
    if allow_cswtch_renames and base.objects is not None and cand.objects is not None:
        base_names = {name for name, _ in base.symbols} | base.plain.keys() | {o.name for o in base.objects}
        cand_names = {name for name, _ in cand.symbols} | cand.plain.keys() | {o.name for o in cand.objects}
        pairs = cswtch_renames(base.objects, cand.objects, base_names, cand_names)
        if pairs:
            renamed = {after: before.name for before, after in pairs}
            # Normalize only the proven OBJECT definitions, in private copies.
            # Extra nm definitions keep their names and undergo ordinary checks.
            objects = copy.copy(cand.objects)
            objects[:] = [replace(obj, name=renamed[obj]) if obj in renamed else obj for obj in cand.objects]
            slots = {(after.name, after.address, after.size): before.name for before, after in pairs}
            symbols = defaultdict(list)
            renamed_keys = set()
            for (name, kind), definitions in cand.symbols.items():
                for address, size in definitions:
                    symbol = slots.get((name, address, size), name)
                    symbols[symbol, kind].append((address, size))
                    if symbol != name:
                        renamed_keys.add((symbol, kind))
            # Preserve nm's tie order on unrelated keys; only renamed groups merge.
            for key in renamed_keys:
                symbols[key].sort()
            cand = replace(cand, objects=objects, symbols=dict(symbols))
            for before, after in pairs:
                report.say(f'WAIVED-CSWTCH-RENAME {before.label} -> {after.name}: '
                           f'{before.address:#x}/{before.size} B; same section, object/TU and bytes')
    if expect_shift:
        check_segment_shifts(base, cand, report)
    object_names = set()
    if base.objects is not None and cand.objects is not None:
        for line in inventory_lines(base.objects, cand.objects):
            report.say(line)
        problems, accepted, allowed = compare_objects(base.objects, cand.objects, object_waivers,
                                                      expect_shift, allow_inventory, aot_objects=aot_objects)
        for problem in problems:
            report.fail(problem)
        for line in accepted:
            report.say(line)
        object_names = {obj.name for obj in base.objects if obj.key in allowed}
    same = Counter()
    late_added = 0
    shifts: dict[str, Counter] = defaultdict(Counter)
    shift_first: dict[tuple[str, int], str] = {}

    def region_of(layout: Layout, addr: int, size: int) -> str:
        section = layout.section_at(addr, size)
        return section.region if section else 'other'

    for key in sorted(base.symbols.keys() | cand.symbols.keys()):
        name, kind = key
        old, new = base.symbols.get(key, []), cand.symbols.get(key, [])
        label = f'{name} ({kind})'
        if old == new:
            for addr, size in old:
                same[region_of(base, addr, size)] += 1
            if full:
                report.say(f'SAME {label}: {[(hex(a), s) for a, s in old]}')
            continue
        qualified_object = name in object_names and all(
            any(obj.name == name and (obj.address, obj.size) == (a, s) for obj in base.objects)
            for a, s in old) and all(
            any(obj.name == name and (obj.address, obj.size) == (a, s) for obj in cand.objects)
            for a, s in new)
        if name in allow or qualified_object:
            report.say(f'ALLOWED {label}: base={[(hex(a), s) for a, s in old]}, '
                       f'candidate={[(hex(a), s) for a, s in new]}')
            continue
        if not old:
            if all(size > 0 and any(lo <= addr and addr + size <= hi for lo, hi in aac_regions)
                   for addr, size in new):
                for addr, size in new:
                    report.say(f'AAC {label}: 0x{addr:08x}, {size} B in newly occupied slot suffix')
                continue
            if all(is_lane_late(cand.section_at(a, s)) for a, s in new):
                late_added += len(new)
                for addr, size in new:
                    report.say(f'LATE {label}: 0x{addr:08x}, {size} B in {cand.section_at(addr, size).name}')
                continue
            if expect_shift and all(region_of(cand, a, s) != 'text' for a, s in new):
                report.say(f'NOTE added {label}: {[(hex(a), s) for a, s in new]}')
                continue
            report.fail(f'added {label}: {[(hex(a), s) for a, s in new]} outside a late section')
            continue
        region = region_of(base, old[0][0], old[0][1])
        if expect_shift and region in ('rodata', 'data', 'bss'):
            if len(old) == len(new) and all(os == ns for (_, os), (_, ns) in zip(old, new)):
                for (oa, _), (na, _) in zip(old, new):
                    delta = na - oa
                    shifts[region][delta] += 1
                    shift_first.setdefault((region, delta), name)
                continue
            if not new:
                report.say(f'NOTE removed {label}: {[(hex(a), s) for a, s in old]}')
                continue
            report.say(f'NOTE resized {label}: base={[(hex(a), s) for a, s in old]}, '
                       f'candidate={[(hex(a), s) for a, s in new]}')
            continue
        moves = ', '.join(f'0x{oa:08x}->0x{na:08x} ({na - oa:+d} B), size {os}->{ns} B'
                          for (oa, os), (na, ns) in zip(old, new))
        if len(old) != len(new):
            moves = f'{len(old)} -> {len(new)} definitions; ' + moves
        report.fail(f'{region} {label}: {moves}')

    # Sections: everything the base has keeps its place, except the late areas and the reserves.
    cand_sections = {s.name: s for s in cand.sections}
    for section in base.sections:
        if section.name in LANE_LATE or is_reserve(section):
            continue
        other = cand_sections.get(section.name)
        if other is None:
            if section.size:
                report.fail(f'section {section.name} is gone')
            continue
        if (other.addr, other.size) == (section.addr, section.size):
            continue
        text = (f'section {section.name}: 0x{section.addr:08x} +{section.size} B -> '
                f'0x{other.addr:08x} +{other.size} B (address {other.addr - section.addr:+d} B, '
                f'size {other.size - section.size:+d} B)')
        if expect_shift and section.region != 'text':
            report.say('SHIFT ' + text)
        else:
            report.fail(text)
    base_names = {s.name for s in base.sections}
    for section in cand.sections:
        if section.name not in base_names and section.name not in LANE_LATE and not is_reserve(section) \
                and section.size:
            report.fail(f'new section {section.name} at 0x{section.addr:08x}, {section.size} B')

    # TLS sections (argbenchr7b R7B-3.1): each keeps its address and size, and none is added.
    base_tls = {s.name: s for s in base.tls or []}
    cand_tls = {s.name: s for s in cand.tls or []}
    for name in sorted(base_tls.keys() | cand_tls.keys()):
        old, new = base_tls.get(name), cand_tls.get(name)
        if old is not None and new is not None and (old.addr, old.size) == (new.addr, new.size):
            continue
        if old is None and new is not None and not new.size:
            continue
        report.fail(f'TLS section {name}: base {(hex(old.addr), old.size) if old else None}, candidate '
                    f'{(hex(new.addr), new.size) if new else None}')

    # A section outside every loaded segment is never mapped (a NOLOAD .late.bss was).
    if cand.segments:
        for section in cand.sections:
            if section.size and not any(v <= section.addr and section.end <= v + m for v, _f, m in cand.segments):
                report.fail(f'section {section.name} (0x{section.addr:08x}, {section.size} B) lies outside every '
                            f'loaded segment')

    for region in ('rodata', 'data', 'bss'):
        if shifts[region]:
            parts = []
            for delta, count in sorted(shifts[region].items()):
                pages = '' if delta % PAGE else f' = {delta // PAGE} pages'
                parts.append(f'{delta:+d} B{pages} x{count} (first {shift_first[(region, delta)]})')
            report.say(f'SHIFT {region} symbols: ' + '; '.join(parts))

    counts = ', '.join(f'{region} {same[region]}' for region in ('text', 'rodata', 'data', 'bss'))
    report.say(f'unchanged symbols: {counts}; added late symbols: {late_added}; failures: {report.failures}')
    return report


def reserve_lines(layout: Layout) -> list[str]:
    lines = []
    have = all(f'__late_{k}_{p}' in layout.plain for k in RESERVES for p in ('start', 'end', 'limit'))
    if have:
        for kind in RESERVES:
            start, end, limit = (layout.plain[f'__late_{kind}_{p}'] for p in ('start', 'end', 'limit'))
            # late_reserve.ld keeps one byte of each reserve so the filler stays PROGBITS: end < limit.
            lines.append(f'reserve {kind}: late code uses {end - start} B from 0x{start:08x}; {limit - end - 1} B left '
                         f'before 0x{limit:08x}')
        lines.append('reserve bss: unbounded (after .bss)')
    else:
        lines.append('no late_reserve.ld symbols: page padding left at the end of each segment')
    names = ('code', 'rodata', 'data')
    for index, (vaddr, _filesz, memsz) in enumerate(layout.segments):
        end = vaddr + memsz
        padding = -end % PAGE
        lines.append(f'segment {names[index] if index < 3 else index}: 0x{vaddr:08x}..0x{end:08x}, '
                     f'page padding {padding} B')
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('base', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--full', action='store_true', help='also list every unchanged symbol')
    parser.add_argument('--allow', action='append', default=[], metavar='NAME',
                        help='accept ordinary symbol changes; pre-existing OBJECTs still need qualified waivers')
    parser.add_argument('--expect-shift', action='store_true',
                        help='a deliberate one-time move: summarise rodata/data/bss moves instead of failing')
    parser.add_argument('--nm')
    parser.add_argument('--readelf')
    parser.add_argument('--base-map', type=Path, help='base link map (otherwise discover beside the ELF)')
    parser.add_argument('--map', type=Path, help='candidate link map (otherwise discover beside the ELF)')
    parser.add_argument('--object-waivers', type=Path, help='JSON with explicit object/section/symbol waivers')
    parser.add_argument('--game', help='game entry to select in the OBJECT waiver manifest')
    parser.add_argument('--allow-inventory', action='store_true',
                        help='acknowledge an audited inventory count change; never waive placement or identity')
    args = parser.parse_args(argv)
    nm, readelf = tool('nm', args.nm), tool('readelf', args.readelf)
    try:
        base, cand = load(args.base, nm, readelf, args.base_map), load(args.candidate, nm, readelf, args.map)
        waivers = waiver_file(args.object_waivers, args.game, args.base, args.candidate) if args.object_waivers else ()
        report = compare(base, cand, frozenset(args.allow), args.expect_shift, args.full, waivers,
                         args.allow_inventory)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'LAYOUT TOOL ERROR: {error}')
        return 2
    for line in report.lines[:-1]:
        print(line)
    for line in reserve_lines(cand):
        print(line)
    print(report.lines[-1])
    return int(report.failures != 0)


if __name__ == '__main__':
    sys.exit(main())
