"""Build the room plan for the RomFS (romfs:/roomplan/<game>.dtp; format: platform/3ds/source/roomplan_format.h).

    py -3 -I tools/roomplan/build_plan.py --game undertale --out <file.dtp> [--data <game folder>] [--records <dir>]
    py -3 -I tools/roomplan/build_plan.py --game deltarune --out <dir> [--data <game folder>] [--records <dir>]

DELTARUNE has a data.win per chapter: --out is a folder, and each chapter with a record gets <chapter folder>.dtp there
(romfs:/roomplan/chapter1_windows.dtp ...); chapters without one get none.

Finds the game's data.win (--data, $CTRPORT_UNDERTALE_DATA, or the Steam library, as tools/aot/prepare.py does),
picks the committed record (tools/roomplan/records/*.json) made on that exact file (SHA-256), resolves its texture
page items to page tiles in the 3DS tile grid, and writes, per scene in first-use order: the tiles (rooms.dtm's
encoding, late when first drawn after the scene's first drawn frame) with their first-use frame, and the code entries
first run from the room's first frame on (read back at the room change on the Old models, phase 2).
Each scene stops at ROOM_TILES_MAX tiles and at the Old 3DS texture budget (counted at 4 bytes a texel, an upper
bound); UNDERTALE's whole-set scenes at twice that (the budget at the 2 bytes a texel most tiles take: the runtime
counts the real formats and loads the start that fits, platform/3ds/source/z_utstall.c rule A, and keeps a RAM copy
of the pages the rest comes from, rule B). A battle room's room-level scene keeps the common tiles of its recorded
fallback groups (all groups unless the record fixes battle_frame_groups to an earlier cohort, preserving the
first-battle preload as coverage grows);
its groups are scenes of their own (their code entries; the runtime picks the group at the room change). Each group's
code also lists its objects' other events and the code SHARED_GROUPS groups ran (battle_code). UNDERTALE's scenes then
list the code their objects can create in play (codeclose.py), and close any normal player walk tile over all four
directions (closure.py); every room but PLACED_ART_SKIP_ROOMS also takes the placed art rule (placed_art), and every battle group the spare puffs (spare_art), BUBBLE_FONT_GROUPS the bubbles' font (bubble_font), every battle group the sprites its own objects switch to (switch_art), and every battle group the art its own objects and what they create can draw (bullet_art). The first of the rooms every UNDERTALE boot walks through (BOOT_ROOMS) also lists the tiles of the ones after it
and of the Settings room (BOOT_AHEAD_SETTINGS), each as its own scene lists them, on the Old models only
(BOOT_AHEAD_MODELS, --model). UNDERTALE's menu room also gets a scene per menu level (menu_levels):
no loader takes them; the story's room load reads the save's level and loads that one's tiles (z_utstall.c's bootahead
block). Rooms without a record get a scene when either closure adds anything. A room-level
record scene's `keep_open_tiles` (UNDERTALE) becomes one more scene of the room, sub
KEEP_SUB, those tiles at LATER and no code: no loader takes it (each picks the room's scene or its battle group's, and
global.battlegroup is never KEEP_SUB), but the Old models' texture-file close after a room load (ctr_textures.c,
CtrTextures_roomLoadedOld) scans every scene of the room, so a cache file whose page has one of those tiles missing
stays open through the room (a room whose preload feeds the next room's restarts). The same inputs always give the
same bytes.

Exit status: 0 written, 3 no game data or no record for it (the build goes on without a plan), 2 an error.
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'aot'))
import closure as closure_mod  # noqa: E402
import codeclose  # noqa: E402
import dwinfo  # noqa: E402

MAGIC = 0x50525444
VERSION = 1
BUILDER = b'roomplan 1'
NO_SUB = 0xFFFF
KEEP_SUB = 0xFFFE           # keep_open_tiles' scene: no loader takes it (module docstring)
TILES_MAX = 512            # ROOM_TILES_MAX (ctr_textures.c)
LATE = 0x80000000
RECORD_FRAMES = 45         # ROOM_RECORD_FRAMES: rooms.dtm keeps the first 45 frames; the runtime seeds those
CAP_BYTES = 20 * 1024 * 1024  # the Old 3DS texture budget is about 20.3 MiB; tiles counted at 4 bytes a texel
# UNDERTALE's whole sets (z_utstall.c rules A and B): 4 bytes a texel is twice what most tiles take (RGBA5551), and
# the 20 MiB cap cut the largest scene's attacks (fought from frame 850 on) and two of its first tiles, which at
# their formats fit with room to spare.
WHOLE_CAP_BYTES = 2 * CAP_BYTES
MAX_SIZE = 1024 * 1024     # ROOMPLAN_MAX_SIZE
LATER = 255                # the bucket of a tile first used after frame 254, or never seen (the whole set)
# A battle room's code that at least this many of its recorded battle groups ran is the battle machinery any fight can
# reach (a kill's dust, sparing, fleeing): every group's scene lists it. The record's UNDERTALE fights: 112 groups,
# only 16 of them ended in a kill, so the other 96 read the kill's code in play, in the frame the monster dies (o3ds:
# two reads, 43 ms). At 10 groups: about 52 more entries a group, 18 KB of bytecode.
SHARED_GROUPS = 10
# The placed art rule (placed_art) is one general rule for every room. PLACED_ART_ROOMS limited it to rooms 41, 67, 120,
# 141 and 263 until the other rooms could be toured; change tilemiss
# (lead GO 11-07, the playtest's in-play page reopens) removes that gate: None means every room. A frozenset names the only
# rooms that take the rule (the tests use it).
PLACED_ART_ROOMS = None
# Rooms whose set would go over the Old 3DS's admission head (the texture budget less ROOMLOAD_SPRITE_HEADROOM_BYTES) with
# the rule's tiles: they keep what they have. Priced at the real formats of both stores' tiles
# (measured): only room 294, the Omega Flowey fight (Steam 140 entries, 25,984 KiB; with the rule's 8 tiles 27,008 KiB,
# over the pool itself). Every other changed room stays at least 8,676 KiB under the head. The builder doesn't price real
# formats, so the list is measured, not computed: the plans are only built for the two data files the records name.
PLACED_ART_SKIP_ROOMS = frozenset((294,))
# Battle groups whose speech bubbles' font (closure.bubble_font_items) no record saw: Asgore's, 100, whose last words
# come after his music stops, where every recording ended (ethanlog4, 11-08: that font's one tile read on the game thread,
# 62 ms, twice, 49 ms of it opening the file).
BUBBLE_FONT_GROUPS = frozenset((100,))
# The switch art rule (switch_art) for every battle group; a frozenset names the only groups that take it (the tests and
# the audit use it). Past SWITCH_ART_MAX_TILES added tiles a group keeps what it has (the largest adds 10 on Steam).
SWITCH_ART_GROUPS = None
SWITCH_ART_MAX_TILES = 24
# The bullet rule (bullet_art) for every battle group; a frozenset names the only groups that take it (the tests use
# it). Past BULLET_ART_MAX_TILES added tiles a group keeps what it has (the largest adds 22 on Steam, 23 on GOG). An object
# more than 1/BULLET_SHARE of the recorded groups' creation walks reach is machinery to the rule too (reach_machinery).
BULLET_ART_GROUPS = None
BULLET_ART_MAX_TILES = 24
BULLET_SHARE = 4
# Past this many added tiles a room keeps what it has (room 67 adds 24 on Steam).
PLACED_ART_MAX_TILES = 24
# The portrait rule (codeclose face_art_sprites, in placed_art) has its own cap, so a room the placed art rule leaves out
# still takes its portraits. Its largest room adds 20 tiles (room 239 on Steam; tilemiss.md).
FACE_ART_MAX_TILES = 24
# UNDERTALE boots through these rooms in this order, whatever the save holds: the story, the title, the menu. The
# first one's whole set (its hidden room load, just after the loading bar) takes the others' tiles too, so pressing
# through the title and menu doesn't wait for them. They'd all be resident by the first real room anyway, so that
# room's texture pool is the same. Tiles only: code groups stay with their own rooms. A later room's tiles are the ones
# its own scene lists: the record's, the whole set's and the placed art rule's. Until change bootahead2 (11-09) the rule
# took only the first two, and the menu's placed art (21 tiles: the pictures behind its words at every level) loaded at
# the title's press instead, in a held frame of 222-233 ms on Azahar Old (change bootahead2).
BOOT_ROOMS = (1, 2, 3)
# The Settings room (room 288): the menu's third choice, so not every boot's, but a press away from the menu on
# any boot. The story's scene takes its tiles too, after the boot rooms'; on Azahar Old its 9 tiles that the boot
# rooms lack loaded at the Settings press, a held frame of 170-206 ms. Its music is not preloaded.
BOOT_AHEAD_SETTINGS = (288,)
# The models the rule is for (build.sh passes --model; a plan built without one keeps the rule). On New consoles it
# bought nothing: the title's and menu's frames were under 33 ms with or without it, and the story's room-change frame
# grew from 419/425 to 556/606 ms behind the bar. The Old models keep it.
BOOT_AHEAD_MODELS = ('o3ds',)
# UNDERTALE's menu (the last of BOOT_ROOMS, room 3) draws a picture behind its words that the save's level picks: 0 to
# 7, how far the save got (platform/3ds/source/z_bootahead.c works it out at boot as the menu's Create does). Levels 2
# to 6 draw these sprites, named by the FNV-1a hash of their names (closure.sprite_hash); 0, 1 and 7 draw nothing the
# room's own scene lacks. Each level gets a scene of the menu's room, sub MENU_LEVEL_SUB + level, every frame of its
# sprites at LATER and no code. No loader takes these (each picks the room's scene or its battle group's, and
# global.battlegroup is never one of them); the story's room load loads the save's level's (z_utstall.c's bootahead
# block, BOOTAHEAD_LEVEL_SUB). The level-6 picture's sprites were loaded at draw time, a shown frame of 83-109 ms on
# Azahar. A level with any sprite missing (another data.win) gets no scene.
MENU_ROOM = 3
MENU_LEVEL_SUB = 0x8000
MENU_LEVELS = {
    2: (0xc858ab8c,),
    3: (0xa5af79ac, 0xb8eb6494, 0x053c51ce),
    4: (0xa5af79ac, 0x720eb1b6, 0x38e6a267, 0x053c51ce),
    5: (0xa5af79ac, 0xa6adf534, 0x720eb1b6, 0x38e6a267, 0xd942f906),
    6: (0xcbcb552c, 0xb8eb6494, 0x8f80b212, 0xf6fddb34, 0x69d8b872, 0x40219492, 0x339cc784, 0x6179532b, 0x8edc6830),
}


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def scene_tiles(info, scene):
    """[(dframe, page, tile)] first use of each tile, in first-use order."""
    first = {}
    for df, tpag in scene['items']:
        page, tiles = info.item_tiles(tpag)
        for t in tiles:
            k = (page, t)
            first[k] = min(df, first.get(k, df))
    for df, tpag, x, y, w, h in scene['parts']:
        page, tiles = info.item_tiles(tpag, (x, y, w, h))
        for t in tiles:
            k = (page, t)
            first[k] = min(df, first.get(k, df))
    return sorted((df, p, t) for (p, t), df in first.items())


def _series(closure, sprite):
    """The other sprites of sprite's numbered series (closure.py), [] for a closure without series."""
    all_series = getattr(closure, 'sprite_series', [])
    return all_series[sprite] if 0 <= sprite < len(all_series) else []


def whole_set(info, closure, room, scene, tiles):
    """UNDERTALE: the tiles the room can draw that its record didn't see, as (LATER, page, tile) after the record's
    (platform/3ds/source/z_utstall.c, item 5): every frame of each sprite the record drew from, and the room's own
    content (tools/roomplan/closure.py). A battle group adds every frame of the sprite of each object whose code the
    group ran, visible or not: a monster's object is invisible, and its sprite is the one drawn when it is hit, a
    texture load on the game thread at the first hit (rule A). A battle group also takes the other sprites of each
    numbered series it draws from or runs the code of (closure.py: Mettaton EX's arm poses). build() passes a
    group's battle_code list, so the objects whose other events or shared code the group may run (the FIGHT target and strike, the dust
    of a kill) count too. Ordinary rooms add only objects with a recorded nonnegative-frame Create: these are
    dynamically created after the room's placed objects were marked. Sorted by page and tile."""
    if closure is None:
        return []
    have = {(p, t) for _df, p, t in tiles}
    extra = set()

    # Tile-level observations can come from the renderer probe when a natural
    # battle reaches art absent from its short TPAG recorder visit. The record
    # is qualified by data SHA and this builder's grid, not by another group's
    # whole sprite union. Preserve ordinary first-use ordering and bucket 255.
    for page, tile in scene.get('observed_late_tiles', ()):
        if (type(page) is not int or type(tile) is not int or
                not 0 <= page < len(info.pages) or info.pages[page] is None or
                not 0 <= tile < info.pages[page].tiles_x * info.pages[page].tiles_y or tile > 255):
            raise ValueError('invalid observed late tile %r' % ((page, tile),))
        extra.add((page, tile))

    def add(tpag, rect=None):
        page, ts = info.item_tiles(tpag, rect)
        if page is not None:
            extra.update((page, t) for t in ts)
    for tpag in {t for _df, t in scene['items']} | {part[1] for part in scene['parts']}:
        for frame in closure.frames(tpag):
            add(frame)
    code_sprite = getattr(closure, 'code_sprite' if 0 <= scene['sub'] < NO_SUB else 'code_create_sprite', [])
    battle = 0 <= scene['sub'] < NO_SUB
    series = set()
    for c in sorted({c for df, c in scene['code'] if df >= 0}):
        s = code_sprite[c] if 0 <= c < len(code_sprite) else -1
        if 0 <= s < len(closure.sprite_items):
            for tpag in closure.sprite_items[s]:
                add(tpag)
            if battle:
                series.update(_series(closure, s))
    if battle:
        # A battle group's drawn sprites and code sprites bring the rest of their numbered series (closure.py).
        sprite_of = getattr(closure, 'sprite_of', {})
        for tpag in {t for _df, t in scene['items']} | {part[1] for part in scene['parts']}:
            s = sprite_of.get(tpag)
            if s is not None:
                series.update(_series(closure, s))
        for s in sorted(series):
            for tpag in closure.sprite_items[s]:
                add(tpag)
    if 0 <= room < len(closure.plan_items):
        for tpag in closure.plan_items[room]:
            add(tpag)
        for tpag, x, y, w, h in closure.plan_parts[room]:
            add(tpag, (x, y, w, h))
    if hasattr(closure, 'walk_tiles'):
        extra.update(closure.walk_tiles(info, have | extra))
    return [(LATER, p, t) for p, t in sorted(extra - have)]


def placed_art(info, closure, code_closure, room, have):
    """The placed art rule: the tiles of every frame of the sprites the room's own objects can draw in play
    (codeclose.CodeClosure.placed_art_sprites) that have isn't, as (LATER, page, tile) sorted by page and tile, after the
    whole set; with them, any rest of the player's walk family they touch. [] with no closures, for a room the gate
    (PLACED_ART_ROOMS) or PLACED_ART_SKIP_ROOMS leaves out; no placed art over PLACED_ART_MAX_TILES counting the walk family, which is never cut short. Mechanism: such a tile
    first drawn in play is a read on the game thread in a dialogue's middle (lab quiz, tile 21:11: a 46 ms frame), and
    its cache file may be closed by then (floor: a 120 ms open). In the whole set it is resident from the room load, and
    evicted as any other room's. Then, under their own cap (FACE_ART_MAX_TILES), the tiles of the portraits any placed
    object's dialogue can open with (codeclose face_art_sprites), even where the placed art is over its cap. This is a partial preload: a computed sprite, room creation code and a path only a
    script reaches are not in the rule, and any such tile still loads on demand in play."""
    if (closure is None or code_closure is None or (PLACED_ART_ROOMS is not None and room not in PLACED_ART_ROOMS) or
            room in PLACED_ART_SKIP_ROOMS):
        return []
    have = set(have)
    extra = set()
    for s in code_closure.placed_art_sprites(room):
        if 0 <= s < len(closure.sprite_items):
            for tpag in closure.sprite_items[s]:
                page, ts = info.item_tiles(tpag)
                if page is not None:
                    extra.update((page, t) for t in ts)
    extra -= have
    if hasattr(closure, 'walk_tiles'):
        extra.update(closure.walk_tiles(info, have | extra) - have)
    if len(extra) > PLACED_ART_MAX_TILES:
        extra = set()
    faces = set()
    if hasattr(code_closure, 'face_art_sprites'):
        for s in code_closure.face_art_sprites(room):
            if 0 <= s < len(closure.sprite_items):
                for tpag in closure.sprite_items[s]:
                    page, ts = info.item_tiles(tpag)
                    if page is not None:
                        faces.update((page, t) for t in ts)
        faces -= have | extra
        if len(faces) > FACE_ART_MAX_TILES:
            faces = set()
    return [(LATER, p, t) for p, t in sorted(extra | faces)]


def spare_art(info, closure, tiles):
    """UNDERTALE: the spare puffs' tiles (closure.spare_items) that tiles hasn't, as (LATER, page, tile) sorted by page
    and tile, for every battle group: any fight can end in a spare, and the puffs were drawn on the game thread at the
    first one (ethanlog3, 11-07: one tile, 21 ms on the playtest card)."""
    have = {(p, t) for _df, p, t in tiles}
    extra = set()
    for tpag in getattr(closure, 'spare_items', ()):
        page, ts = info.item_tiles(tpag)
        if page is not None:
            extra.update((page, t) for t in ts)
    return [(LATER, p, t) for p, t in sorted(extra - have)]


def machinery_objects(code_closure, subs):
    """The objects whose code at least SHARED_GROUPS of the recorded battle groups ran (code_closure.owner): the battle
    machinery every fight runs, whose art belongs to no one group (switch_art leaves it out). An empty set without a
    code closure."""
    owner = getattr(code_closure, 'owner', None)
    if not owner:
        return set()
    counts = {}
    for s in subs:
        for o in {owner[c] for _df, c in s['code'] if c in owner}:
            counts[o] = counts.get(o, 0) + 1
    return {o for o, k in counts.items() if k >= SHARED_GROUPS}


def switch_art(info, closure, code_closure, sub, code, machinery, tiles):
    """UNDERTALE: the tiles of the sprites a battle group's own objects can switch to that tiles hasn't, as (LATER, page,
    tile) sorted by page and tile, for a group SWITCH_ART_GROUPS takes; [] without both closures, or past
    SWITCH_ART_MAX_TILES. A group's own objects are the owners of its code list (battle_code) less the machinery
    (machinery_objects); their sprites, every frame, are each constant their own events (no parent's) pass to a
    draw_sprite function or store in a variable named *sprite* (codeclose: sprite_index in a Step, an Alarm or a User
    event; the spared or hurt look). Whole sets take the sprite an object starts with and the ones a record saw; a switch
    first drawn in play was a read on the game thread (lead, 11-08: Endogeny's body at the fourth pet, one page-23 tile
    read 6 ms after the switch; on the player's card the open alone is about 51 ms). Also the constants an object's
    events store in a plain variable they draw or take as the sprite (codeclose taken_sprites: So Sorry's arm, Ice
    Cap's hurt look, read mid-fight, changes escapes and tilemiss 11-08). A computed sprite is not in the rule."""
    if (closure is None or code_closure is None or not hasattr(code_closure, 'owner') or
            (SWITCH_ART_GROUPS is not None and sub not in SWITCH_ART_GROUPS)):
        return []
    owner = code_closure.owner
    have = {(p, t) for _df, p, t in tiles}
    sprites = set()
    for o in sorted({owner[c] for _df, c in code if c in owner} - set(machinery)):
        if hasattr(code_closure, 'taken_sprites'):
            sprites.update(code_closure.taken_sprites(o))
        for c in code_closure.object_codes(o, parents=False):
            if not 0 <= c < len(code_closure.draws):
                continue
            sprites.update(code_closure.draws[c])
            for name, values in code_closure.any_stores[c].items():
                if 'sprite' in name:
                    sprites.update(values)
    extra = set()
    for s in sprites:
        if 0 <= s < len(closure.sprite_items):
            for tpag in closure.sprite_items[s]:
                page, ts = info.item_tiles(tpag)
                if page is not None:
                    extra.update((page, t) for t in ts)
    extra -= have
    if len(extra) > SWITCH_ART_MAX_TILES:
        return []
    return [(LATER, p, t) for p, t in sorted(extra)]


def reach_machinery(code_closure, subs):
    """The objects more than 1/BULLET_SHARE of the recorded battle groups reach (code_closure.battle_objects of each
    group's recorded code): the generic bullets, the writer, the weapons. An empty set without a code closure."""
    if not hasattr(code_closure, 'battle_objects'):
        return set()
    groups = [{c for df, c in s['code'] if df >= 0} for s in subs]
    reach = {}
    for seeds in groups:
        for o in code_closure.battle_objects(seeds):
            reach[o] = reach.get(o, 0) + 1
    return {o for o, n in reach.items() if n * BULLET_SHARE > len(groups)}


def bullet_art(info, closure, code_closure, sub, seeds, machinery, tiles, core=None):
    """UNDERTALE: the tiles of the art a battle group's own objects and what they create can draw
    (codeclose.CodeClosure.created_art_sprites of the group's recorded code, less machinery) that tiles hasn't, as
    (LATER, page, tile) sorted by page and tile, for a group BULLET_ART_GROUPS takes; [] without both closures, or past
    BULLET_ART_MAX_TILES. The record lists what its fights drew before the recording stopped, and a recording stops: an
    attack first seen in play was a read on the game thread (lead, 11-08: every group plans the art its code can
    reach). A computed sprite and an object created from a computed value are not in the rule. With core
    (machinery_objects), the rule takes the generators' cases and the scripts' creations too (created_art_sprites);
    a group that would pass BULLET_ART_MAX_TILES that way gets the rule without them, as before (change tilemiss, 11-08:
    the attacks a recording never reached were read from the SD card in play in 14 groups, change escapes)."""
    if (closure is None or code_closure is None or not hasattr(code_closure, 'created_art_sprites') or
            (BULLET_ART_GROUPS is not None and sub not in BULLET_ART_GROUPS)):
        return []
    have = {(p, t) for _df, p, t in tiles}
    for wide in ((core,) if core is None else (core, None)):
        extra = set()
        for s in code_closure.created_art_sprites(seeds, machinery, wide):
            if 0 <= s < len(closure.sprite_items):
                for tpag in closure.sprite_items[s]:
                    page, ts = info.item_tiles(tpag)
                    if page is not None:
                        extra.update((page, t) for t in ts)
        extra -= have
        if len(extra) <= BULLET_ART_MAX_TILES:
            return [(LATER, p, t) for p, t in sorted(extra)]
    return []


def boot_ahead(info, closure, by_room, room, tiles, model=None, code_closure=None):
    """UNDERTALE: the tiles of the BOOT_ROOMS after room and of BOOT_AHEAD_SETTINGS, as (LATER, page, tile) after
    tiles, each once, in the order those rooms' own scenes list them (record, whole set, placed art); [] for any other
    room or without a closure (DELTARUNE)."""
    if closure is None or room != BOOT_ROOMS[0] or (model is not None and model not in BOOT_AHEAD_MODELS):
        return []
    have = {(p, t) for _df, p, t in tiles}
    out = []
    for later in BOOT_ROOMS[1:] + BOOT_AHEAD_SETTINGS:
        for s in by_room.get(later, ()):
            if s['sub'] < 0:
                first = scene_tiles(info, s)
                own = first + whole_set(info, closure, later, s, first)
                own += placed_art(info, closure, code_closure, later, {(p, t) for _df, p, t in own})
                for _df, p, t in own:
                    if (p, t) not in have:
                        have.add((p, t))
                        out.append((LATER, p, t))
    return out


def menu_levels(info, closure, room):
    """UNDERTALE: [(sub, tiles)] for the menu's levels (MENU_LEVELS), tiles as [(LATER, page, tile)] in sprite and
    frame order, each once; [] for any other room or without a closure (DELTARUNE)."""
    by_hash = getattr(closure, 'sprite_hash', None)
    if closure is None or room != MENU_ROOM or not by_hash:
        return []
    out = []
    for level, hashes in sorted(MENU_LEVELS.items()):
        sprites = [by_hash.get(h) for h in hashes]
        if any(s is None or not 0 <= s < len(closure.sprite_items) for s in sprites):
            continue
        seen, tiles = set(), []
        for s in sprites:
            for item in closure.sprite_items[s]:
                page, ts = info.item_tiles(item)
                for t in ts if page is not None else ():
                    if (page, t) not in seen:
                        seen.add((page, t))
                        tiles.append((LATER, page, t))
        if tiles:
            out.append((MENU_LEVEL_SUB + level, tiles))
    return out


def capped(info, tiles, cap=CAP_BYTES):
    out, total = [], 0
    for df, p, t in tiles:
        b = info.tile_bytes(p)
        if len(out) >= TILES_MAX or total + b > cap:
            break
        out.append((df, p, t))
        total += b
    return out, total


def battle_code(info, subs):
    """{sub: [(dframe, code)]} for a battle room's groups: each group's own code, then its objects' other events (an
    object whose event ran can reach the rest: a monster's Destroy at the kill), then the code SHARED_GROUPS groups
    ran, most shared first. The added entries count as first run at LATER."""
    own = {s['sub']: [(df, c) for df, c in s['code'] if df >= 0] for s in subs}
    counts = {}
    for code in own.values():
        for c in {c for _df, c in code}:
            counts[c] = counts.get(c, 0) + 1
    shared = sorted((c for c, k in counts.items() if k >= SHARED_GROUPS), key=lambda c: (-counts[c], c))
    owners = getattr(info, 'code_owner', [])
    events = {}
    for c, o in enumerate(owners):
        if o is not None:
            events.setdefault(o, []).append(c)
    out = {}
    for sub, code in own.items():
        have = {c for _df, c in code}
        extra = []
        for o in sorted({owners[c] for c in have if 0 <= c < len(owners) and owners[c] is not None}):
            extra += [c for c in events[o] if c not in have]
        have.update(extra)
        extra += [c for c in shared if c not in have]
        out[sub] = code + [(LATER, c) for c in extra]
    return out


def keep_tiles(info, scene):
    """A room-level scene's keep_open_tiles as [(LATER, page, tile)], sorted (module docstring); [] without any."""
    out = set()
    for page, tile in scene.get('keep_open_tiles', ()):
        if (type(page) is not int or type(tile) is not int or
                not 0 <= page < len(info.pages) or info.pages[page] is None or
                not 0 <= tile < info.pages[page].tiles_x * info.pages[page].tiles_y or tile > 255):
            raise ValueError('invalid keep-open tile %r' % ((page, tile),))
        out.add((LATER, page, tile))
    return sorted(out)


def encode_scene(info, room, sub, tiles, code, cap=CAP_BYTES):
    """tiles: [(dframe, page, tile)] in order. Late: first drawn after the scene's first drawn frame."""
    tiles, total = capped(info, tiles, cap)
    first = tiles[0][0] if tiles else 0
    entries = [(LATE if df > first else 0) | p << 8 | t for df, p, t in tiles]
    buckets = bytes(min(df, 255) for df, _p, _t in tiles)
    # Code first run in the room change itself (frame -1: the new room's Create events) has run before the read-ahead.
    code = [c for df, c in code if df >= 0 and 0 <= c < 0x10000]
    body = struct.pack('<HHHHI', room, sub, len(entries), len(code), total)
    body += struct.pack('<%dI' % len(entries), *entries)
    body += buckets + b'\0' * (-len(buckets) % 4)
    cb = struct.pack('<%dH' % len(code), *code)
    body += cb + b'\0' * (-len(cb) % 4)
    return body


def in_play(code_closure, room, plan_code, record_code, battle=False):
    """UNDERTALE's in-play code closure (codeclose.py): the entries to add after a scene's code, at LATER."""
    if code_closure is None:
        return []
    added, _bytes = code_closure.extras(room, [c for df, c in plan_code if df >= 0], [c for _df, c in record_code],
                                        battle)
    return [(LATER, c) for c in added]


def bubble_font(info, closure, sub, tiles):
    """UNDERTALE: the speech bubbles' font tiles (closure.bubble_font_items) that tiles hasn't, as (LATER, page, tile)
    sorted by page and tile, for a battle group in BUBBLE_FONT_GROUPS; else none."""
    if sub not in BUBBLE_FONT_GROUPS:
        return []
    have = {(p, t) for _df, p, t in tiles}
    extra = set()
    for tpag in getattr(closure, 'bubble_font_items', ()):
        page, ts = info.item_tiles(tpag)
        if page is not None:
            extra.update((page, t) for t in ts)
    return [(LATER, p, t) for p, t in sorted(extra - have)]


def build(info, record, closure=None, code_closure=None, model=None):
    """closure: a closure.Closure for UNDERTALE's whole sets, or None (DELTARUNE: the record's tiles only).
    code_closure: a codeclose.CodeClosure for UNDERTALE's in-play code, or None.
    model: the target ('o3ds' or 'n3ds'), for the rules that are a model's own (BOOT_AHEAD_MODELS); None keeps them.
    A room with no record gets only those walk tiles and in-play code, when either adds anything."""
    scenes = []
    cap = WHOLE_CAP_BYTES if closure is not None else CAP_BYTES
    by_room = {}
    for s in record['scenes']:
        by_room.setdefault(s['room'], []).append(s)
    rooms = set(by_room)
    walks = {}
    if closure is not None and hasattr(closure, 'walk_frames'):
        walks = {room: closure.walk_frames(room) for room in range(info.room_count)}
        rooms.update(room for room, items in walks.items() if items)
    if code_closure is not None:
        rooms.update(range(min(info.room_count, len(code_closure.insts_by_room))))
    for room in sorted(rooms):
        if room not in by_room:
            extra = in_play(code_closure, room, [], [])
            # A room with no recorder visit still needs its placed player's walk closure.
            tiles = sorted({(LATER, p, t) for item in walks.get(room, ())
                            for p, ts in [info.item_tiles(item)] if p is not None for t in ts})
            tiles += placed_art(info, closure, code_closure, room, {(p, t) for _df, p, t in tiles})
            if extra or tiles:
                scenes.append(encode_scene(info, room, NO_SUB, tiles, extra, cap))
            continue
        group = by_room[room]
        whole = [s for s in group if s['sub'] < 0]
        subs = sorted((s for s in group if s['sub'] >= 0), key=lambda s: s['sub'])
        if whole:
            s = whole[0]
            tiles = scene_tiles(info, s)
            # The whole set reads the record's code (code_create_sprite): the in-play extras go to the code list only.
            code = s['code'] + ([] if subs else in_play(code_closure, room, s['code'], s['code']))
            tiles = tiles + whole_set(info, closure, room, s, tiles)
            tiles += placed_art(info, closure, code_closure, room, {(p, t) for _df, p, t in tiles})
            scenes.append(encode_scene(info, room, NO_SUB,
                tiles + boot_ahead(info, closure, by_room, room, tiles, model, code_closure), code, cap))
            keep = keep_tiles(info, s)
            if keep:
                scenes.append(encode_scene(info, room, KEEP_SUB, keep, []))
            for sub, level in menu_levels(info, closure, room):
                scenes.append(encode_scene(info, room, sub, level, [], cap))
        elif subs:
            # New outlier fights must not shrink an established first-battle preload.
            frame_groups = record.get('battle_frame_groups', {}).get(str(room))
            frame_subs = subs
            if frame_groups is not None:
                by_sub = {s['sub']: s for s in subs}
                if not frame_groups or len(set(frame_groups)) != len(frame_groups) or any(g not in by_sub for g in frame_groups):
                    raise ValueError('invalid battle_frame_groups for room %d' % room)
                frame_subs = [by_sub[g] for g in sorted(frame_groups)]
            per = [{(p, t): df for df, p, t in scene_tiles(info, s)} for s in frame_subs]
            common = set(per[0])
            for m in per[1:]:
                common &= set(m)
            tiles = sorted((max(m[k] for m in per), k[0], k[1]) for k in common)
            scenes.append(encode_scene(info, room, NO_SUB, tiles, []))
        codes = battle_code(info, subs)
        machinery = machinery_objects(code_closure, subs)
        bullet_machinery = machinery | reach_machinery(code_closure, subs)
        for s in subs:
            if 0 <= s['sub'] < NO_SUB:
                tiles = scene_tiles(info, s)
                # Rule A reads the group's whole code list (battle_code): the battle's shared objects' sprites too.
                whole = whole_set(info, closure, room, dict(s, code=codes[s['sub']]), tiles)
                code = codes[s['sub']] + in_play(code_closure, room, codes[s['sub']], s['code'], battle=True)
                whole += bubble_font(info, closure, s['sub'], tiles + whole)
                whole += spare_art(info, closure, tiles + whole)
                whole += switch_art(info, closure, code_closure, s['sub'], codes[s['sub']], machinery, tiles + whole)
                whole += bullet_art(info, closure, code_closure, s['sub'], {c for df, c in s['code'] if df >= 0},
                                    bullet_machinery, tiles + whole, machinery)
                scenes.append(encode_scene(info, room, s['sub'], tiles + whole, code, cap))
    body = b''.join(scenes)
    head = struct.pack('<II32sIIQIIIIII16s', MAGIC, VERSION, bytes.fromhex(info.sha256), info.size & 0xFFFFFFFF,
                       info.game_id, info.timestamp, dwinfo.TILE_MAX, dwinfo.GRID_VERSION, info.room_count, len(scenes),
                       len(body), fnv1a(body), BUILDER)
    assert len(head) == 96
    data = head + body
    if len(data) > MAX_SIZE:
        raise ValueError('plan of %d bytes is over %d' % (len(data), MAX_SIZE))
    return data


def find_record(records_dir, sha):
    if not os.path.isdir(records_dir):
        return None
    for name in sorted(os.listdir(records_dir)):
        if name.endswith('.json'):
            with open(os.path.join(records_dir, name), 'r', encoding='ascii') as f:
                record = json.load(f)
            if record.get('data_sha256') == sha:
                return record
    return None


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='undertale', choices=['undertale', 'deltarune'])
    ap.add_argument('--data', help='the game folder (default: as tools/aot/prepare.py finds it)')
    ap.add_argument('--data-win', help='the data.win itself (tests)')
    ap.add_argument('--records', default=os.path.join(HERE, 'records'))
    ap.add_argument('--out', required=True)
    ap.add_argument('--halfart', help='prototype (build.sh halfart=): the remap tools/analysis/halfart_pack.py wrote')
    ap.add_argument('--model', choices=['o3ds', 'n3ds'],
                    help='the target (build.sh): the boot rooms rule is for BOOT_AHEAD_MODELS only; without it, kept')
    a = ap.parse_args(argv)
    if a.data_win is not None:
        found = [('root', a.data_win, 0)]
    else:
        import prepare  # tools/aot/prepare.py: the same search as the AOT step
        import corpus
        folder = prepare.find_data(a.game, a.data)
        found = corpus.parts(a.game, folder) if folder else []
    found = [f for f in found if os.path.isfile(f[1])]
    if not found:
        print('Room plan: no %s data.win found' % a.game)
        return 3
    if a.game == 'undertale':
        return write_plan(found[0][1], a.records, a.out, whole=True, halfart=a.halfart, model=a.model)
    written = 0
    for part, path, _chapter in found:
        rc = write_plan(path, a.records, os.path.join(a.out, part + '.dtp'))
        if rc == 2:
            return 2
        written += rc == 0
    return 0 if written else 3


def write_plan(path, records, out, whole=False, halfart=None, model=None):
    """0 written, 3 no record for this data.win, 2 a record that doesn't fit. whole: add each scene's whole set (UNDERTALE)."""
    info = dwinfo.DataWinInfo(path)
    record = find_record(records, info.sha256)
    if record is None:
        print('Room plan: no record for %s (SHA-256 %s)' % (path, info.sha256))
        return 3
    if record.get('format') != 1 or record.get('room_count') != info.room_count or record.get('tpag_count') != len(info.tpag):
        print('Room plan: the record for SHA-256 %s does not fit this data.win' % info.sha256, file=sys.stderr)
        return 2
    if halfart:
        # The halved items' new rectangles (and so tiles), as the console patches them.
        import json
        with open(halfart, encoding='utf-8') as f:
            remap = json.load(f)
        for key, item in remap.items():
            t = list(info.tpag[int(key)])
            assert t[0:4] == item['old'][0:4] and t[6:8] == item['old'][4:6], key
            t[0:4], t[6:8] = item['new'][0:4], item['new'][4:6]
            info.tpag[int(key)] = tuple(t)
        print('Room plan: %d half-resolution items from %s' % (len(remap), halfart))
    closure = None
    if whole:
        closure = closure_mod.Closure(path)
        if not closure.ok:
            print('Room plan: no whole sets (not a GameMaker 1.x data.win this builder can read)')
            closure = None
    code_closure = None
    if whole:
        code_closure = codeclose.CodeClosure(path)
        if not code_closure.ok:
            print('Room plan: no in-play code closure (not a GameMaker 1.x data.win this builder can read)')
            code_closure = None
        elif code_closure.face_note:
            print('Room plan: placed art takes no portraits: %s' % code_closure.face_note)
    data = build(info, record, closure, code_closure, model)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, out)
    print('Room plan: %s, %d scenes, %d bytes, record %s %s %s' % (out, struct.unpack_from('<I', data, 68)[0], len(data),
                                                              record['game'], record['store'], record['version']))
    return 0

if __name__ == '__main__':
    sys.exit(main())
