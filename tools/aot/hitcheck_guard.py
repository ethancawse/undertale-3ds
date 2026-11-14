"""Fail UT builds that need the deliberately omitted PC region specialization.

GM 1.4 resolves builtin calls from FUNC, including calls in unselected AOT code.
Reject either name even with zero recorded occurrences. The generic shared VM
builtins remain intact; DELTARUNE does not run this game-specific gate.
"""
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gmlaot import DataWin

REGIONS = frozenset(('instance_activate_region', 'instance_deactivate_region'))


def check_data(path):
    data = DataWin(str(path))
    if data.wad != 16:
        raise ValueError('hitcheck trim requires UNDERTALE WAD 16')
    forbidden = REGIONS.intersection(row['name'] for row in data.funcs)
    if forbidden:
        raise ValueError('UT references ' + ', '.join(sorted(forbidden)) +
                         '; restore and qualify the PC region reader before building')
    return len(data.funcs)


def main(argv=None):
    import argparse
    import prepare
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data')
    args = parser.parse_args(argv)
    folder = prepare.find_data('undertale', args.data)
    if not folder:
        print('hitcheck region guard: no UT data found', file=sys.stderr)
        return 3
    try:
        count = check_data(Path(folder) / 'data.win')
    except (OSError, ValueError, KeyError, IndexError, struct.error, SystemExit) as error:
        print('Refusing to build: hitcheck region guard: ' + str(error), file=sys.stderr)
        return 2
    print(f'hitcheck region guard: {count} FUNC entries; neither region builtin referenced')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
