"""Turn room plan recorder logs (roomplan-rec.txt, platform/3ds/source/rp_recorder.inc) into a committed record.

    py -3 -I tools/roomplan/records.py --data <data.win> --store steam --version 1.08 --out <record.json>
        [--source "<what was played>"] [--missing-from <base.json>] [--code-only] <roomplan-rec.txt ...>

The record holds asset numbers only: per scene (a room, or a battle room and its battle group), each texture page item
first drawn (T: whole items; S: a background's part) with its frame since the room change, and the code entries used,
merged over every visit in every log: the earliest frame wins. It is keyed to the data.win's SHA-256, so the builder
uses it only for that exact file. No names, pixels or code. Tile lines (P) are checked against what the items resolve
to and summarised, never stored: tiles depend on the page layout, items don't.
"""
import argparse
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'aot'))
import dwinfo  # noqa: E402
import corpus  # noqa: E402

FORMAT = 1
# Rooms whose visits are told apart by battle group (UNDERTALE's one battle room).
BATTLE_ROOMS = {'undertale': ('room_battle',)}


def fingerprint(record):
    """Source-content identity, independent of whitespace and dictionary insertion order."""
    return hashlib.sha256(json.dumps(record, sort_keys=True, separators=(',', ':')).encode('ascii')).hexdigest()


def require_store_parity(steam, gog):
    """Reject a stale or incomplete atlas reprojection before accepting its coverage."""
    if gog.get('source_record_sha256') != fingerprint(steam):
        raise ValueError('GOG room record is not projected from the current Steam record')
    keys = []
    for record in (steam, gog):
        rows = [(s['room'], s['sub']) for s in record['scenes']]
        if len(set(rows)) != len(rows):
            raise ValueError('Duplicate room/group scene')
        keys.append(set(rows))
    if keys[0] != keys[1]:
        raise ValueError('Steam/GOG room scenes or battle groups differ')
    for a, b in zip(sorted(steam['scenes'], key=lambda s: (s['room'], s['sub'])),
                    sorted(gog['scenes'], key=lambda s: (s['room'], s['sub']))):
        if (a['visits'], a['code']) != (b['visits'], b['code']):
            raise ValueError('Steam/GOG visit or code coverage differs')


def read_logs(paths, counts=None):
    """[(room, battlegroup, {('T', tpag) | ('S', tpag, x, y, w, h) | ('C', code): dframe}, {(page, tile): dframe})] per visit.

    counts: (rooms, tpag, codes) of the data.win wanted; visits after an H line for another data.win are left out
    (DELTARUNE: one log holds every chapter played, each chapter's visits after its own H line)."""
    visits = []
    for path in paths:
        cur = None
        keep = True
        with open(path, 'r', encoding='ascii', errors='replace') as f:
            for line in f:
                w = line.split()
                if not w:
                    continue
                try:
                    if w[0] == 'H' and len(w) >= 11:
                        cur = None
                        keep = counts is None or (int(w[4]), int(w[6]), int(w[10])) == tuple(counts)
                    elif not keep:
                        continue
                    elif w[0] == 'V' and len(w) == 5:
                        cur = (int(w[2]), int(w[3]), {}, {})
                        visits.append(cur)
                    elif cur is None:
                        continue
                    elif w[0] == 'T' and len(w) == 3:
                        key, df = ('T', int(w[2])), int(w[1])
                        cur[2][key] = min(df, cur[2].get(key, df))
                    elif w[0] == 'S' and len(w) == 7:
                        key, df = ('S',) + tuple(int(x) for x in w[2:7]), int(w[1])
                        cur[2][key] = min(df, cur[2].get(key, df))
                    elif w[0] == 'C' and len(w) == 3:
                        key, df = ('C', int(w[2])), int(w[1])
                        cur[2][key] = min(df, cur[2].get(key, df))
                    elif w[0] == 'P' and len(w) == 4:
                        key, df = (int(w[2]), int(w[3])), int(w[1])
                        cur[3][key] = min(df, cur[3].get(key, df))
                except ValueError:
                    continue  # a line cut short when the run ended
    return visits


def build_record(info, room_names, game, visits, store, version, sources):
    battle = {i for i, n in enumerate(room_names) if n in BATTLE_ROOMS.get(game, ())}
    scenes = {}
    checked = missed = 0
    for room, group, uses, tiles in visits:
        if not 0 <= room < info.room_count:
            continue
        key = (room, group if room in battle and group >= 0 else -1)
        s = scenes.setdefault(key, {'visits': 0, 'uses': {}})
        s['visits'] += 1
        for k, df in uses.items():
            s['uses'][k] = min(df, s['uses'].get(k, df))
        # Check: every tile the cache was asked for in the visit's first frames is one the items resolve to.
        got = set()
        for k in uses:
            if k[0] == 'T':
                page, ts = info.item_tiles(k[1])
            elif k[0] == 'S':
                page, ts = info.item_tiles(k[1], k[2:6])
            else:
                continue
            got.update((page, t) for t in ts)
        for pt, df in tiles.items():
            if df <= 45:
                checked += 1
                missed += pt not in got
    out = []
    for (room, sub) in sorted(scenes):
        s = scenes[(room, sub)]
        u = sorted(s['uses'].items(), key=lambda kv: (kv[1], kv[0]))
        out.append({'room': room, 'sub': sub, 'visits': s['visits'],
                    'items': [[df, k[1]] for k, df in u if k[0] == 'T'],
                    'parts': [[df] + list(k[1:]) for k, df in u if k[0] == 'S'],
                    'code': [[df, k[1]] for k, df in u if k[0] == 'C']})
    record = {'format': FORMAT, 'game': game, 'store': store, 'version': version, 'data_sha256': info.sha256,
              'data_size': info.size, 'room_count': info.room_count, 'tpag_count': len(info.tpag),
              'code_count': info.code_count, 'sources': sources, 'scenes': out}
    return record, checked, missed


def record_bytes(record):
    # One scene per line: small diffs, stable bytes.
    head = {k: v for k, v in record.items() if k != 'scenes'}
    lines = ['{']
    for k, v in head.items():
        lines.append('  %s: %s,' % (json.dumps(k), json.dumps(v, sort_keys=True)))
    lines.append('  "scenes": [')
    for i, s in enumerate(record['scenes']):
        lines.append('    ' + json.dumps(s, separators=(',', ':')) + (',' if i + 1 < len(record['scenes']) else ''))
    lines.append('  ]')
    lines.append('}')
    return ('\n'.join(lines) + '\n').encode('ascii')


def write_record(record, path):
    with open(path, 'wb') as f:
        f.write(record_bytes(record))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', required=True, help='the data.win the logs were recorded on')
    ap.add_argument('--game', default='undertale')
    ap.add_argument('--store', required=True)
    ap.add_argument('--version', required=True)
    ap.add_argument('--source', action='append', default=[])
    ap.add_argument('--missing-from', help='snapshot only battle scenes absent from this pinned base record')
    ap.add_argument('--code-only', action='store_true', help='snapshot code observations with no duplicate draw indices')
    ap.add_argument('--out', required=True)
    ap.add_argument('logs', nargs='+')
    a = ap.parse_args(argv)
    info = dwinfo.DataWinInfo(a.data)
    visits = read_logs(a.logs, (info.room_count, len(info.tpag), info.code_count))
    record, checked, missed = build_record(info, corpus.room_names(a.data), a.game, visits, a.store, a.version, a.source)
    if a.missing_from:
        with open(a.missing_from, encoding='ascii') as f:
            base = json.load(f)
        if base['data_sha256'] != info.sha256:
            raise ValueError('--missing-from base does not match data.win')
        keys = {(s['room'], s['sub']) for s in base['scenes']}
        record['scenes'] = [s for s in record['scenes'] if s['sub'] >= 0 and (s['room'], s['sub']) not in keys]
    if a.code_only:
        for scene in record['scenes']:
            scene['items'] = []
            scene['parts'] = []
    write_record(record, a.out)
    rooms = len({s['room'] for s in record['scenes']})
    subs = sum(1 for s in record['scenes'] if s['sub'] >= 0)
    print('records: %d visits, %d scenes (%d rooms of %d, %d battle groups), %d items, %d parts, %d code entries'
          % (len(visits), len(record['scenes']), rooms, info.room_count, subs,
             sum(len(s['items']) for s in record['scenes']), sum(len(s['parts']) for s in record['scenes']),
             sum(len(s['code']) for s in record['scenes'])))
    print('tile check (first 45 frames): %d tiles asked for, %d not covered by the recorded items' % (checked, missed))
    return 0


if __name__ == '__main__':
    sys.exit(main())
