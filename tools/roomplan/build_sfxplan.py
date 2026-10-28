"""Build UNDERTALE's sound plan for the RomFS (romfs:/roomplan/undertale-sfx.dtp; format and runtime:
platform/3ds/source/z_utsfx.c).

    py -3 -I tools/roomplan/build_sfxplan.py --out <file.dtp> [--data <game folder>] [--records <dir>] [--plan-records <dir>]

Finds the game's data.win (--data, $CTRPORT_UNDERTALE_DATA, or the Steam library, as tools/aot/prepare.py does), picks
the committed sound record (tools/roomplan/records/sounds/*.json, made by sound_records.py) made on that exact file
(SHA-256), and writes per scene (a room; a battle room's battle group) the sounds it played that the 3DS copies into its
sound cache at their first play: embedded PCM WAVs read into memory (under 512 KiB in data.win; bigger ones stream from
their section) whose copy fits the cache entry cap. Each entry carries its copy's size (stereo WAVs whose channels are
identical are kept as mono, as ctr_audio_system.c's buildSfx does), and each scene keeps within SCENE_BYTES, in first-play
order; each scene also says how many of them lead its first FIRST_FRAMES frames (the 3DS copies those in the room
change's frame, the rest on a worker thread). The same inputs always give the same bytes.

Kill sounds (utfirstuse): every battle scene also plans KILL_SOUNDS after its recorded sounds, so the sound
UNDERTALE plays at every monster kill is copied on the worker during the battle rather than read and copied on the
game thread in the kill's frame (7-8 ms on the Old 2DS with its data read ahead). A battle room is one the record
lists battle groups in; a group the record has no scene for takes the room's scene, which gets those sounds alone when
the record has none for the room itself. They come last, within SCENE_BYTES like the rest.

Hurt sounds (utfirstuse): a monster sets its hurt sound in its Create event and plays it from that variable when
hit, so a recording sees it only in the groups it fought (group 51's: a 5 ms read and copy at the first hit in
the others). hurt_sounds reads it from the bytecode: for each battle group in the room plan's record (--plan-records,
build_plan.py's, which lists the code each group ran), the constant each of its objects' Create events stores to the
HURT_VARS variable. Those come after the group's recorded sounds and before KILL_SOUNDS, on the worker; a group the
sound record has no scene for gets a scene of them.

Exit status: 0 written, 3 no game data or no record for it (the build goes on without a plan), 2 an error.
"""
import argparse
import hashlib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'aot'))

MAGIC = 0x50535444            # "DTSP" (UTSFX_MAGIC)
VERSION = 2
NO_SUB = 0xFFFF
SECTION_MIN = 512 * 1024      # AUDO_SECTION_MIN_BYTES: from this size a sound streams from its section of data.win
ENTRY_MAX = 2 * 1024 * 1024   # SFX_CACHE_MAX_ENTRY_BYTES
SCENE_BYTES = 2 * 1024 * 1024  # copies planned per scene at most (the cache holds 6 MiB; Old linear memory is tight)
MAX_SIZE = 256 * 1024         # UTSFX_MAX_SIZE
FIRST_FRAMES = 10             # a sound first played before this frame of the visit is copied in the room change's frame
FLAG_EMBEDDED = 0x01
FLAG_COMPRESSED = 0x02
FLAG_REGULAR = 0x64
# Planned in every battle scene (see above): the SHA-1 of each sound's name (the game's names stay out of the source).
KILL_SOUNDS = ('f76722798e48e2149950ae7cd3a4344eb184fb9b',)
# The variable a monster's Create event stores its hurt sound in (hurt_sounds), by the SHA-1 of its name.
HURT_VARS = ('ef9769fc4b5d3fde444079123b464f9fae3d5559',)
LATER = 1 << 30               # the frame given to a kill sound: after the first frames, so copied on the worker


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def chunks(d):
    out = {}
    o = 8
    while o + 8 <= len(d):
        name = d[o:o + 4].decode('ascii', 'replace')
        size = struct.unpack_from('<I', d, o + 4)[0]
        out[name] = (o + 8, size)
        o += 8 + size
    return out


def wav_copy_bytes(w):
    """The bytes buildSfx copies for a WAV blob, or None if the 3DS doesn't copy it (not PCM 8/16-bit mono/stereo,
    empty, or over the cache entry cap)."""
    if len(w) < 12 or w[:4] != b'RIFF' or w[8:12] != b'WAVE':
        return None
    k = 12
    fmt = None
    while k + 8 <= len(w):
        cid = w[k:k + 4]
        length = struct.unpack_from('<I', w, k + 4)[0]
        if cid == b'fmt ' and length >= 16:
            tag, ch, rate = struct.unpack_from('<HHI', w, k + 8)
            bits = struct.unpack_from('<H', w, k + 22)[0]
            fmt = (tag, ch, rate, bits)
        elif cid == b'data':
            start = k + 8
            data_bytes = min(length, len(w) - start)
            break
        k += 8 + length + (length & 1)
    else:
        return None
    if fmt is None:
        return None
    tag, ch, rate, bits = fmt
    if tag != 1 or ch not in (1, 2) or bits not in (8, 16) or rate == 0:
        return None
    if data_bytes > ENTRY_MAX:
        return None
    bps = bits // 8
    frames = data_bytes // (ch * bps)
    if frames == 0:
        return None
    out_ch = ch
    if ch == 2:
        s = w[start:start + frames * 2 * bps]
        if bps == 2:
            dual = s[0::4] == s[2::4] and s[1::4] == s[3::4]
        else:
            dual = s[0::2] == s[1::2]
        if dual:
            out_ch = 1
    return frames * out_ch * bps


def read_sounds(path):
    """Per SOND index: {'name', 'copy'} where copy is the bytes the 3DS copies into its sound cache at the first play
    (None: not a sound the plan can take)."""
    with open(path, 'rb') as f:
        d = f.read()
    ch = chunks(d)

    def string(p):
        n = struct.unpack_from('<I', d, p - 4)[0]
        return d[p:p + n].decode('utf-8', 'replace')

    so, _ = ch['SOND']
    n = struct.unpack_from('<I', d, so)[0]
    ptrs = struct.unpack_from('<%dI' % n, d, so + 4)
    ao, alen = ch['AUDO']
    na = struct.unpack_from('<I', d, ao)[0]
    aptrs = struct.unpack_from('<%dI' % na, d, ao + 4)
    aend = ao + alen
    out = []
    for p in ptrs:
        name = string(struct.unpack_from('<I', d, p)[0])
        flags = struct.unpack_from('<I', d, p + 4)[0]
        group = struct.unpack_from('<i', d, p + 28)[0]
        af = struct.unpack_from('<i', d, p + 32)[0]
        regular = (flags & FLAG_REGULAR) == FLAG_REGULAR
        in_audo = not regular or (flags & (FLAG_EMBEDDED | FLAG_COMPRESSED)) != 0
        copy = None
        if in_audo and group == 0 and 0 <= af < na:
            q = aptrs[af]
            size = struct.unpack_from('<I', d, q)[0]
            # The lazy parser may take the distance to the next entry as the size (AudioEntry.sizeIsUpperBound).
            bound = (aptrs[af + 1] if af + 1 < na else aend) - q - 4
            if 0 < size and max(size, bound) < SECTION_MIN:
                copy = wav_copy_bytes(d[q + 4:q + 4 + size])
        out.append({'name': name, 'copy': copy})
    return out


def hurt_sounds(path, info, plan_record):
    """{(room, sub): [sound]} for the battle groups of the room plan's record (made on this data.win): the hurt sound
    each of the group's monsters sets in its Create event (a constant stored to a HURT_VARS variable), in object order.
    A monster plays it when hit (UNDERTALE sets it per monster: one constant, then played from a variable, so a
    recording sees it only in groups it fought). Empty if the bytecode can't be read."""
    import gmlaot  # tools/aot/gmlaot.py
    try:
        dw = gmlaot.DataWin(path)
        dw.parse_code()
        dw.parse_vari()
        dw.parse_func()
        dw.patch_operands()
    except (SystemExit, gmlaot.Reject, KeyError, ValueError, struct.error, IndexError):
        return {}
    hurt_var = {i for i, v in enumerate(dw.vars) if hashlib.sha1(v['name'].encode('utf-8')).hexdigest() in HURT_VARS}
    owners = getattr(info, 'code_owner', [])
    by_name = {e['name']: e for e in dw.code if e is not None}

    def object_hurt(obj):
        e = by_name.get(obj.decode('utf-8', 'replace') + '_Create_0')  # obj: dwinfo.code_owner's gml_Object_<object>
        out = []
        if e is None or e['offset'] != 0:
            return out
        a, end, k = e['abs'], e['abs'] + e['length'], None
        try:
            while a < end:
                ins = gmlaot.Instr(dw.data, a)
                if ins.op == gmlaot.OP_POP and k is not None and ins.var_index() in hurt_var and k not in out:
                    out.append(k)
                k = ins.inst if ins.op == gmlaot.OP_PUSHI else None
                a = ins.next
        except gmlaot.Reject:
            return []
        return out

    cache, out = {}, {}
    for s in plan_record.get('scenes', []):
        if s.get('sub', -1) < 0:
            continue
        objects = []
        for df, c in s.get("code", []):
            if df < 0:
                continue  # run before the group started (build_plan.battle_code counts df >= 0 as the group's own)
            o = owners[c] if 0 <= c < len(owners) else None
            if o is not None and o not in objects:
                objects.append(o)
        found = []
        for o in sorted(objects):
            if o not in cache:
                cache[o] = object_hurt(o)
            found += [k for k in cache[o] if k not in found]
        if found:
            out[(s['room'], s['sub'])] = found
    return out


def with_battle_sounds(record_scenes, sounds, hurt=None):
    """The record's scenes with each battle group's hurt sounds (hurt_sounds: those the record's scene doesn't list,
    after its own) and then KILL_SOUNDS added last to each battle scene; a group with hurt sounds but no recorded
    scene gets a scene of them; the battle room gets its own scene of KILL_SOUNDS when the record has none for it."""
    hurt = hurt or {}
    kills = [k for k, snd in enumerate(sounds) if hashlib.sha1(snd['name'].encode('utf-8')).hexdigest() in KILL_SOUNDS]
    battle = {s['room'] for s in record_scenes if s['sub'] >= 0} | {room for room, _sub in hurt}
    out = []
    for s in record_scenes:
        if s['room'] in battle:
            have = [k for _df, k in s['sounds']]
            for k in hurt.get((s['room'], s['sub']), []) + kills:
                if 0 <= k < len(sounds) and k not in have:
                    have.append(k)
                    s = dict(s, sounds=list(s['sounds']) + [[LATER, k]])
        out.append(s)
    recorded = {(s['room'], s['sub']) for s in record_scenes}
    for room, sub in sorted(hurt):
        if (room, sub) not in recorded:
            ks = []
            for k in hurt[(room, sub)] + kills:
                if 0 <= k < len(sounds) and k not in ks:
                    ks.append(k)
            out.append({'room': room, 'sub': sub, 'sounds': [[LATER, k] for k in ks]})
    for room in sorted(battle - {s['room'] for s in record_scenes if s['sub'] < 0}):
        if kills:
            out.append({'room': room, 'sub': -1, 'sounds': [[LATER, k] for k in kills]})
    return out


def build(info, sounds, record, hurt=None):
    """The plan's bytes. info: dwinfo.DataWinInfo; sounds: read_sounds(); record: a sound record for this data.win;
    hurt: hurt_sounds() or None."""
    scenes = []
    for s in with_battle_sounds(record['scenes'], sounds, hurt):
        room, sub = s['room'], s['sub']
        if not 0 <= room < info.room_count:
            continue
        entries = []
        total = 0
        first = 0
        for df, k in s['sounds']:
            copy = sounds[k]['copy'] if 0 <= k < len(sounds) else None
            if copy is None or total + copy > SCENE_BYTES:
                continue
            total += copy
            entries.append((k, (copy + 1023) // 1024))
            if df < FIRST_FRAMES and first == len(entries) - 1:
                first += 1
        if entries:
            scenes.append((room, NO_SUB if sub < 0 else sub, entries, first))
    scenes.sort(key=lambda t: (t[0], t[1]))
    body = bytearray()
    for room, sub, entries, first in scenes:
        body += struct.pack('<HHHH', room, sub, len(entries), first)
        for k, kb in entries:
            body += struct.pack('<HH', k, kb)
    head = struct.pack('<II32sIIIIII', MAGIC, VERSION, bytes.fromhex(info.sha256), len(sounds), info.room_count,
                       len(scenes), len(body), fnv1a(body), 0)
    assert len(head) == 64
    data = head + bytes(body)
    if len(data) > MAX_SIZE:
        raise ValueError('sound plan of %d bytes is over %d' % (len(data), MAX_SIZE))
    return data


def find_record(records_dir, sha):
    if not os.path.isdir(records_dir):
        return None
    for name in sorted(os.listdir(records_dir)):
        if name.endswith('.json'):
            with open(os.path.join(records_dir, name), 'r', encoding='ascii') as f:
                record = json.load(f)
            if record.get('kind') == 'sounds' and record.get('data_sha256') == sha:
                return record
    return None


def find_plan_record(plan_records, sha):
    """The room plan's record for this data.win (records/*.json, build_plan.py's), whose battle groups hurt_sounds
    reads, or None."""
    if not plan_records or not os.path.isdir(plan_records):
        return None
    for name in sorted(os.listdir(plan_records)):
        if name.endswith('.json'):
            with open(os.path.join(plan_records, name), 'r', encoding='ascii') as f:
                record = json.load(f)
            if record.get('kind') != 'sounds' and record.get('data_sha256') == sha:
                return record
    return None


def write_plan(path, records, out, plan_records=None):
    """0 written, 3 no record for this data.win, 2 a record that doesn't fit."""
    import dwinfo
    info = dwinfo.DataWinInfo(path)
    record = find_record(records, info.sha256)
    if record is None:
        print('Sound plan: no record for %s (SHA-256 %s)' % (path, info.sha256))
        return 3
    sounds = read_sounds(path)
    if record.get('format') != 1 or record.get('room_count') != info.room_count or record.get('sound_count') != len(sounds):
        print('Sound plan: the record for SHA-256 %s does not fit this data.win' % info.sha256, file=sys.stderr)
        return 2
    plan_record = find_plan_record(plan_records, info.sha256)
    hurt = hurt_sounds(path, info, plan_record) if plan_record is not None else {}
    print('Sound plan: hurt sounds for %d battle groups (%d sounds)' % (len(hurt), sum(len(v) for v in hurt.values())))
    data = build(info, sounds, record, hurt)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, out)
    count = struct.unpack_from('<I', data, 48)[0]
    print('Sound plan: %s, %d scenes, %d bytes, record %s %s %s' % (out, count, len(data), record['game'], record['store'],
                                                                  record['version']))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', help='the game folder (default: as tools/aot/prepare.py finds it)')
    ap.add_argument('--data-win', help='the data.win itself (tests)')
    ap.add_argument('--records', default=os.path.join(HERE, 'records', 'sounds'))
    ap.add_argument('--plan-records', default=os.path.join(HERE, 'records'), help="the room plan's records (hurt sounds)")
    ap.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    if a.data_win is not None:
        path = a.data_win
    else:
        import prepare  # tools/aot/prepare.py: the same search as the AOT step
        import corpus
        folder = prepare.find_data('undertale', a.data)
        found = corpus.parts('undertale', folder) if folder else []
        path = found[0][1] if found else None
    if path is None or not os.path.isfile(path):
        print('Sound plan: no undertale data.win found')
        return 3
    try:
        return write_plan(path, a.records, a.out, a.plan_records)
    except (ValueError, KeyError, struct.error) as e:
        print('Sound plan: %s' % e, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
