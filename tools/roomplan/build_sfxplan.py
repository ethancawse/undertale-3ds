"""Build UNDERTALE's sound plan for the RomFS (romfs:/roomplan/undertale-sfx.dtp; format and runtime:
platform/3ds/source/z_utsfx.c).

    py -3 -I tools/roomplan/build_sfxplan.py --out <file.dtp> [--data <game folder>] [--records <dir>] [--plan-records <dir>]
        [--streams]

Finds the game's data.win (--data, $CTRPORT_UNDERTALE_DATA, or the Steam library, as tools/aot/prepare.py does), picks
the committed sound record (tools/roomplan/records/sounds/*.json, made by sound_records.py) made on that exact file
(SHA-256), and writes per scene (a room; a battle room's battle group) the sounds it played that the 3DS copies into its
sound cache at their first play: embedded PCM WAVs read into memory (under 512 KiB in data.win; bigger ones stream from
their section) whose copy fits the cache entry cap. Each entry carries its copy's size (stereo WAVs whose channels are
identical are kept as mono, as ctr_audio_system.c's buildSfx does), and each scene keeps within SCENE_BYTES, in first-play
order; each scene also says how many of them lead its first FIRST_FRAMES frames (the 3DS copies those in the room
change's frame, the rest on a worker thread). The same inputs always give the same bytes.

Kill sounds (change utfirstuse): every battle scene also plans KILL_SOUNDS after its recorded sounds, so the sound
UNDERTALE plays at every monster kill is copied on the worker during the battle rather than read and copied on the
game thread in the kill's frame (7-8 ms on the Old 2DS with its data read ahead). A battle room is one the record
lists battle groups in; a group the record has no scene for takes the room's scene, which gets those sounds alone when
the record has none for the room itself. They come last, within SCENE_BYTES like the rest.

Hurt sounds (change utfirstuse): a monster sets its hurt sound in its Create event and plays it from that variable when
hit, so a recording sees it only in the groups it fought (group 51's: a 5 ms read and copy at the first hit in
the others). hurt_sounds reads it from the bytecode: for each battle group in the room plan's record (--plan-records,
build_plan.py's, which lists the code each group ran), the constant each of its objects' Create events stores to the
HURT_VARS variable. Those come after the group's recorded sounds and before KILL_SOUNDS, on the worker; a group the
sound record has no scene for gets a scene of them.

Code sounds (change tilemiss): the sound record lists what our recordings heard, and a recording can stop before a fight
does: Mettaton EX's stopped 697 frames in, so the bombs, the heart's hits and the heart's explosion were read or decoded
at their first play (Old 3DS, 11-08: a big frame drop as the heart first exploded). code_sounds reads each battle
group's sounds from the bytecode instead (codeclose.py's battle_sounds: the group's code in the room plan's record, what
it creates, each constant passed to a sound-playing call), leaving out the common machinery that most groups reach (an
object more than 1/MACHINERY_SHARE of the groups reach: the dialogue writer's voices, the weapons' hits, the generic
bullets). They come after the group's kill sounds, on the worker, within SCENE_BYTES like the rest; one already
listed isn't listed twice, and one the 3DS doesn't copy (a file of its own, or a big one) is left out here.

Stream scenes (change omegaflowey, --streams: UNDERTALE Old builds): the sound record's "streams" list (stream_records.py,
from our playthrough logs) names, per room, the big compressed sounds that room decoded into the sound cache. Each
room gets a scene of them with sub STREAM_SUB (entries u16 sound, u16 0), which the Old runtime reads at the room
change to have them stream for the room (z_utsfx.c's usPreloadOld) instead of taking a linear cache copy. Scene
lookups for a battle group never match that sub, so other runtimes skip the scene.

Warm sounds (change omegaflowey W1, --streams: UNDERTALE Old builds): the sound record's "warm" list
(stream_records.py --warm, from our playthrough logs) names, per room, the short embedded Ogg sound effects whose plays
opened a stream over and over before their background decode came in (Omega Flowey's flame loop: one restart per
flame, about every 67 ms, for the 1.7 s the Old models took to decode it). Each comes first in the room's scene (its
decoded size from the Ogg headers, inside `first`), so the room change's preload asks for its decode then
(ctr_audio_system.c's flamePreload) and flowend keeps the copy for the room.

Boot sounds (change firstuse2): the record lists a sound only in the rooms and battle groups our tours played it in, so
the common ones of every session (the encounter's alert and the soul's fall, the save point's two, the attack's slash,
the player's and the enemy's hurt, the menu's choice and move, the game over's two breaks) were read at their first
play in any other room: a read on the audio loader thread, whose first read of the session also opens data.win (54-262
ms an open on an SD card measured on an Old 3DS), so the sound starts late. BOOT_SOUNDS go after the recorded sounds
of each BOOT_ROOMS scene (the intro story and the title menu, which every launch loads, New Game and Continue alike),
on the worker: a best-effort prefetch under the plan's usual rules (SCENE_BYTES, the 6 MiB cache cap counting queued
copies, the worker's free-memory floor). The worker drops what it hadn't started at the next room change, and the
title menu asks again for what is missing. A copy can be declined, and a cached one can go later: as a cold entry it
is an early pick for LRU eviction, and texture shedding takes it once it is 10 s old, keeping the cache's 2 MiB
minimum. While cached it counts against the cap, so a later room's plan copy can be declined where it would have
fitted. A sound not in the cache at its play loads then, as before.

Voices (change papvoice): UNDERTALE's dialogue writer stops and plays its voice at every letter, so a voice not in the
cache lost a line's first blips while its read was on its way (each letter's stop dropped the waiting voice; Papyrus's
"HUMAN." in the fog room lost up to all six). The record lists a voice only where our tours first read it, so the
census (voice_census.py) finds each room's and battle group's text voices in the bytecode: the typers its code sets and
the `\\T` codes of the text it names. They go first on the worker in their scene (after the room change's own copies),
smallest first, each once, so a line at a scene's start waits for one small read: a copy the cache can't take now is
declined, and a cached one is as cold as any planned copy. Each has VOICE_KIB set in its size: the runtime
(ctr_audio_system.c's CtrAudioSystem_queuePlanned) queues it without the flag and outside flowend's FLOWEND_KEEP
kept sounds, which stay the ones the scene keeps without its voices (the first that many of its other entries). One
more scene (room
VOICE_ROOM, sub VOICE_SUB, which no scene lookup asks for: z_utsfx.c reads it by that sub) lists every text voice whose
read is VOICE_DIRECT_MAX bytes or less: the 3DS reads one of those at its play on the game thread, so it starts at
the letter that asked for it (a voice the census missed, or a planned one the worker had not finished).

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
STREAM_SUB = 0xFFFE              # UTSFX_STREAM_SUB (z_utsfx.c): the room's sounds that stream
VOICE_SUB = 0xFFFD               # UTSFX_VOICE_SUB (z_utsfx.c): the text voices read at their play (change papvoice)
VOICE_ROOM = 0                   # the room the voice scene is filed under (any room: no lookup asks for its sub)
VOICE_DIRECT_MAX = 32 * 1024     # a voice read at its play has a WAV of this many bytes at most
VOICE_KIB = 0x8000               # set in an added voice's size: queued unflagged, not one of flowend's kept
FLOWEND_KEEP = 8                 # ctr_audio_system.c: a scene's first this many entries are kept from shedding
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
MACHINERY_SHARE = 4           # code_sounds: an object more than 1/4 of the battle groups reach adds no sounds
LATER = 1 << 30               # the frame given to a kill sound: after the first frames, so copied on the worker
# Boot sounds (see above), by the SHA-1 of each name, in the order the worker copies them: the encounter's two, the save
# point's two, the attack, the two hurts, the menu's two, the game over's two.
BOOT_SOUNDS = ('433fc66aa67b4ed3c04f312317df45da949c2a44', 'f746c8d8f323f7427b524584faedca5613d86c78',
               '4c8952cbf07d1effb7d21d47819fa3887ee681aa', '4c752e880240ec0d7ca3bfdd462338fbca9c2769',
               'c3a031e43c78538f26d4a8f9ee4d11983c036aac', 'cc617967655b453b1c5c551efbfe469d30f0af4b',
               'f3fe84697ef008972dd2233cab76d15001e2773f',
               '4e9d5d5d4a22e4766158e0e29ad8550448131ca5', '0576409aaec72859aad84b140a81ce96a6d18d4a',
               '5859960fac293f94b052f1b5a762de79837d192d', '3f3cf6e327074053411654bda902bd721bfee4cd')
BOOT_ROOMS = (1, 3)           # UNDERTALE's intro story (room 0 is the logo) and title menu


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


WARM_MAX = 256 * 1024         # a warm sound's decoded copy at most (flowhitch's floor: bigger ones are shed for good)


def ogg_copy_bytes(b):
    """The bytes of the cache copy the 3DS decodes an Ogg Vorbis blob into (16-bit, its channels: total frames from
    the last page's granule position), or None if it isn't a mono or stereo Ogg Vorbis file."""
    if len(b) < 58 or b[:4] != b'OggS':
        return None
    p = 27 + b[26]
    if b[p:p + 7] != b'\x01vorbis' or p + 12 > len(b):
        return None
    ch = b[p + 11]
    last = b.rfind(b'OggS')
    if ch not in (1, 2) or last + 14 > len(b):
        return None
    frames = struct.unpack_from('<q', b, last + 6)[0]
    return frames * ch * 2 if frames > 0 else None


def read_sounds(path):
    """Per SOND index: {'name', 'copy', 'warm'} where copy is the bytes the 3DS copies into its sound cache at the
    first play (None: not a sound the plan can take), and warm the bytes of the decoded copy of a compressed sound
    in AUDO (an Ogg under SECTION_MIN, decoded to at most WARM_MAX; else None)."""
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
        copy = warm = blob = None
        if in_audo and group == 0 and 0 <= af < na:
            q = aptrs[af]
            size = struct.unpack_from('<I', d, q)[0]
            # The lazy parser may take the distance to the next entry as the size (AudioEntry.sizeIsUpperBound).
            bound = (aptrs[af + 1] if af + 1 < na else aend) - q - 4
            if 0 < size and max(size, bound) < SECTION_MIN:
                blob = size
                copy = wav_copy_bytes(d[q + 4:q + 4 + size])
                if flags & FLAG_COMPRESSED:
                    warm = ogg_copy_bytes(d[q + 4:q + 4 + size])
                    warm = warm if warm is not None and warm <= WARM_MAX else None
        out.append({'name': name, 'copy': copy, 'warm': warm, 'blob': blob})
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


def code_sounds(path, plan_record):
    """{(room, sub): [sound]} for the battle groups of the room plan's record (made on this data.win): the sounds their
    code can play (codeclose.CodeClosure.battle_sounds, seeded with the code each group ran in the record), the common
    machinery left out (an object more than 1/MACHINERY_SHARE of the groups reach). Empty if the bytecode can't be read."""
    import codeclose
    cc = codeclose.CodeClosure(path)
    if not cc.ok:
        return {}
    groups = []
    for s in plan_record.get('scenes', []):
        if s.get('sub', -1) >= 0:
            groups.append(((s['room'], s['sub']), {c for df, c in s.get('code', []) if df >= 0}))
    reach = {}
    for _key, seeds in groups:
        for o in cc.battle_objects(seeds):
            reach[o] = reach.get(o, 0) + 1
    machinery = frozenset(o for o, n in reach.items() if n * MACHINERY_SHARE > len(groups))
    out = {}
    for key, seeds in groups:
        found = cc.battle_sounds(seeds, machinery)
        if found:
            out[key] = found
    return out


def with_battle_sounds(record_scenes, sounds, hurt=None, code=None):
    """The record's scenes with each battle group's hurt sounds (hurt_sounds: those the record's scene doesn't list,
    after its own), then KILL_SOUNDS, then its code sounds (code_sounds) added last to each battle scene; a group with
    hurt or code sounds but no recorded scene gets a scene of them; the battle room gets its own scene of KILL_SOUNDS
    when the record has none for it."""
    hurt = hurt or {}
    code = code or {}
    kills = [k for k, snd in enumerate(sounds) if hashlib.sha1(snd['name'].encode('utf-8')).hexdigest() in KILL_SOUNDS]
    battle = {s['room'] for s in record_scenes if s['sub'] >= 0} | {room for room, _sub in hurt} | {room for room, _sub in code}
    out = []
    for s in record_scenes:
        if s['room'] in battle:
            have = [k for _df, k in s['sounds']]
            for k in hurt.get((s['room'], s['sub']), []) + kills + code.get((s['room'], s['sub']), []):
                if 0 <= k < len(sounds) and k not in have:
                    have.append(k)
                    s = dict(s, sounds=list(s['sounds']) + [[LATER, k]])
        out.append(s)
    recorded = {(s['room'], s['sub']) for s in record_scenes}
    for room, sub in sorted(set(hurt) | set(code)):
        if (room, sub) not in recorded:
            ks = []
            for k in hurt.get((room, sub), []) + kills + code.get((room, sub), []):
                if 0 <= k < len(sounds) and k not in ks:
                    ks.append(k)
            out.append({'room': room, 'sub': sub, 'sounds': [[LATER, k] for k in ks]})
    for room in sorted(battle - {s['room'] for s in record_scenes if s['sub'] < 0}):
        if kills:
            out.append({'room': room, 'sub': -1, 'sounds': [[LATER, k] for k in kills]})
    return out


def with_boot_sounds(scenes, sounds):
    """The scenes with BOOT_SOUNDS after each BOOT_ROOMS room's own (sub -1) sounds, on the worker, each once; a boot room
    gets a scene of them when it has none. Nothing changes when none of them is in this data.win."""
    by_hash = {hashlib.sha1(snd['name'].encode('utf-8')).hexdigest(): k for k, snd in enumerate(sounds)}
    boot = [by_hash[h] for h in BOOT_SOUNDS if h in by_hash]
    if not boot:
        return scenes
    out = list(scenes)
    for room in BOOT_ROOMS:
        at = next((i for i, s in enumerate(out) if s['room'] == room and s['sub'] < 0), None)
        if at is None:
            out.append({'room': room, 'sub': -1, 'sounds': []})
            at = len(out) - 1
        have = [k for _df, k in out[at]['sounds']]
        out[at] = dict(out[at], sounds=list(out[at]['sounds']) + [[LATER, k] for k in boot if k not in have])
    return out


def voice_scenes(path, plan_record):
    """({(room, sub): [sound]}, [sound]): each scene's text voices (voice_census.py: every room; each battle group of
    the room plan's record, from the code it ran) and every text voice; ({}, []) if the census can't read the data."""
    import voice_census
    vc = voice_census.VoiceCensus(path)
    if not vc.ok:
        print('Sound plan: no voice census (%s)' % vc.note)
        return {}, []
    scenes = (plan_record or {}).get('scenes', [])
    out = {}
    for room in range(len(vc.cc.insts_by_room)):
        code = [c for s in scenes if s['room'] == room and s.get('sub', -1) < 0 for _df, c in s.get('code', [])]
        out[(room, -1)] = vc.scene_voices(room, code)
    for s in scenes:
        if s.get('sub', -1) >= 0:
            code = [c for df, c in s.get('code', []) if df >= 0]   # the group's own (as hurt_sounds)
            out[(s['room'], s['sub'])] = vc.scene_voices(s['room'], code, battle=True)
    return {k: v for k, v in out.items() if v}, vc.all_voices()


def with_voice_sounds(scenes, sounds, voices):
    """The scenes with each scene's voices (voice_scenes) first on the worker, after the room change's own copies, each
    once; a scene the others don't have gets one of them."""
    out, have_keys = [], set()
    for s in scenes:
        key = (s['room'], s['sub'])
        have_keys.add(key)
        ks = [k for k in voices.get(key, []) if 0 <= k < len(sounds) and sounds[k]['copy'] is not None]
        have = [k for _df, k in s['sounds']]
        add = [k for k in ks if k not in have]
        # 'voices': how many of the scene's last sounds are these (build() copies them first on the worker).
        out.append(dict(s, sounds=list(s['sounds']) + [[LATER, k] for k in add], voices=len(add)) if add else s)
    for key in sorted(voices):
        if key not in have_keys:
            ks = [k for k in voices[key] if 0 <= k < len(sounds) and sounds[k]['copy'] is not None]
            if ks:
                out.append({'room': key[0], 'sub': key[1], 'sounds': [[LATER, k] for k in ks], 'voices': len(ks)})
    return out


def build(info, sounds, record, hurt=None, streams=False, voices=None, direct=None, code=None):
    """The plan's bytes. info: dwinfo.DataWinInfo; sounds: read_sounds(); record: a sound record for this data.win;
    hurt: hurt_sounds() or None; streams: add the record's stream scenes and warm sounds (UNDERTALE Old); voices,
    direct: voice_scenes()'s two (None: no voices); code: code_sounds() or None."""
    warm = {}
    for w in (record.get('warm', []) if streams else []):
        ks = [k for k in w['sounds'] if 0 <= k < len(sounds) and sounds[k].get('warm')]
        if ks and 0 <= w['room'] < info.room_count:
            warm[w['room']] = ks
    scenes = []
    rooms = with_boot_sounds(with_battle_sounds(record['scenes'], sounds, hurt, code), sounds)
    if voices:
        rooms = with_voice_sounds(rooms, sounds, voices)
    rooms += [{'room': r, 'sub': -1, 'sounds': []} for r in sorted(warm) if not any(s['room'] == r and s['sub'] < 0 for s in rooms)]
    for s in rooms:
        room, sub = s['room'], s['sub']
        if not 0 <= room < info.room_count:
            continue
        # The room's warm sounds first, all in its first frames (preloadSfx asks for their decode at the room change).
        ks = warm.get(room, []) if sub < 0 else []
        entries = [(k, (sounds[k]['warm'] + 1023) // 1024) for k in ks]
        total = sum(sounds[k]['warm'] for k in ks)
        first = len(entries)
        voice_at, voice_entries = len(s['sounds']) - s.get('voices', 0), []
        for i, (df, k) in enumerate(s['sounds']):
            copy = sounds[k]['copy'] if 0 <= k < len(sounds) else None
            if copy is None or total + copy > SCENE_BYTES:
                continue
            total += copy
            if i >= voice_at:
                voice_entries.append((k, (copy + 1023) // 1024))
                continue
            entries.append((k, (copy + 1023) // 1024))
            if df < FIRST_FRAMES and first == len(entries) - 1:
                first += 1
        # The voices with_voice_sounds added: chosen last (the scene's other sounds keep their place under the
        # budget), copied first on the worker, so a line at the scene's start waits for one small read, not for the
        # copies ahead of it (change papvoice: maintainer's card took 105.7 ms for one voice read while a save was written).
        # VOICE_KIB keeps them out of flowend's kept sounds (R435 F2): the scene keeps the same ones as without them.
        entries[first:first] = [(k, kb | VOICE_KIB) for k, kb in sorted(voice_entries, key=lambda e: e[1])]   # the smallest first
        if entries:
            scenes.append((room, NO_SUB if sub < 0 else sub, entries, first))
    for st in (record.get('streams', []) if streams else []):
        ks = [k for k in st['sounds'] if 0 <= k < len(sounds)]
        if ks and 0 <= st['room'] < info.room_count:
            scenes.append((st['room'], STREAM_SUB, [(k, 0) for k in ks], 0))
    reads = [k for k in (direct or []) if 0 <= k < len(sounds) and sounds[k]['copy'] is not None
             and sounds[k].get('blob') is not None and sounds[k]['blob'] <= VOICE_DIRECT_MAX]
    if reads:
        scenes.append((VOICE_ROOM, VOICE_SUB, [(k, (sounds[k]['copy'] + 1023) // 1024) for k in reads], 0))
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


def write_plan(path, records, out, plan_records=None, streams=False):
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
    voices, direct = voice_scenes(path, plan_record)
    print('Sound plan: voices for %d scenes (%d entries), %d voices' % (len(voices), sum(len(v) for v in voices.values()),
                                                                     len(direct)))
    code = code_sounds(path, plan_record) if plan_record is not None else {}
    print('Sound plan: code sounds for %d battle groups (%d sounds)' % (len(code), sum(len(v) for v in code.values())))
    data = build(info, sounds, record, hurt, streams, voices, direct, code)
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
    ap.add_argument('--streams', action='store_true', help="add the record's stream scenes (UNDERTALE Old builds)")
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
        return write_plan(path, a.records, a.out, a.plan_records, a.streams)
    except (ValueError, KeyError, struct.error) as e:
        print('Sound plan: %s' % e, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
