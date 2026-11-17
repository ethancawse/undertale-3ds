"""UNDERTALE AAC slot capacities, one copy for the tools (N10 rider R9, review decision 11-15).

platform/3ds/aacout.cmake is the build's copy; test_aac_capacity.py proves the two agree, so a capacity change edits
aacout.cmake and this file and nothing else that gates current builds.

TEXT is each model's AAC text slot today:
  Old 80,260 B since the Old 1.0 envelope (relready-resize.md, integrate21 dv8q, 11-09);
  New 59,780 B since N10 rider R9 (+16 KiB from New's late reserve; review decision 11-15).
TEXT_RETIRED holds the text capacities that frozen builds still carry: 43,396 B was New's slot until N10 (and every
model's before dv8q). A layout pair must still have the same extent on both sides (check_layout), so a pair across a
capacity change fails there by design; that move is accepted once, by ruling, and recorded with its cause.
RODATA is the AAC rodata slot as first built, both models (the 23,530 B vendor payload plus its 2 B internal
alignment); New keeps it. RODATA_CURRENT is each model's rodata slot today: Old 7,148 B since the one-time [R]
reserve exchange (oldspace, N14; 16,384 B of unused AAC rodata padding became Old ordinary late text).
RODATA_RETIRED holds the rodata capacities frozen builds still carry (Old 23,532 B, N13 and earlier).
KNOWN_PAIRS are the (text, rodata) slot pairs a link may have; 7,148 B goes only with Old's 80,260 B text slot.
Knowing a pair grants no transition: a layout pair still needs one extent on both sides, and the [R] move itself
was admitted once, by its hash-bound proof (oldspace-r-admission.md).

Historical proofs keep their own literals on purpose, because they check frozen inputs from before R9:
check_aacout_layout.py (the retired vendor payload, 43,396 B), check_touchquiz_aac.py, check_touchui_dv8q.py and
test_touch_sampler_owner.py.

OLD_DEVONLY_TAIL is plain dev's dev-only AAC text tail cap; OLD_PLAIN_FREE is what plain Old dev keeps free after it
(2,048 B since the 11-16 ruling for framelead; 4,096 B from the 11-15 N13 ruling L2, 8,192 B before); OLD_RESERVE, their sum, is the protected AAC text reserve every
UNDERTALE Old link keeps after its production lists (lead 11-14, audit F3-1 T7). Release measures it at dev's production
end: the release-only latereclaim block and its phase pad do not count (lead 11-15, N13 ruling L1).
"""
TEXT = {'o3ds': 80260, 'n3ds': 59780}
OLD_DEVONLY_TAIL = 8192
OLD_PLAIN_FREE = 2048
OLD_RESERVE = OLD_DEVONLY_TAIL + OLD_PLAIN_FREE
# The reserve before the 11-15 N13 L1/L2 rulings: frozen links (N12 and earlier) carry it in their latereclaim ASSERT.
OLD_RESERVE_RETIRED = 16384
# The reserve from the 11-15 N13 L2 ruling to the 11-16 framelead ruling (8,192 + 4,096 B): frozen N13/N14 links carry it.
OLD_RESERVE_N13 = 12288
TEXT_RETIRED = (43396,)
TEXT_KNOWN = tuple(sorted(set(TEXT.values()) | set(TEXT_RETIRED)))
RODATA = 23532
RODATA_CURRENT = {'o3ds': 7148, 'n3ds': RODATA}
RODATA_RETIRED = {'o3ds': (RODATA,), 'n3ds': ()}
RODATA_KNOWN = tuple(sorted(set(RODATA_CURRENT.values()) | {c for v in RODATA_RETIRED.values() for c in v}))
KNOWN_PAIRS = frozenset({(43396, RODATA), (TEXT['n3ds'], RODATA), (TEXT['o3ds'], RODATA),
                         (TEXT['o3ds'], RODATA_CURRENT['o3ds'])})


def rodata_capacities(model):
    """The model's rodata slot today, then the retired ones frozen builds carry."""
    return (RODATA_CURRENT[model], *RODATA_RETIRED[model])


def known_capacity_pair(text, rodata):
    return (text, rodata) in KNOWN_PAIRS
