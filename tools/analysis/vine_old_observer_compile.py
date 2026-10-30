"""Route Stage 2 of X-F1-Full prototype through frozen emitted entry operands."""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vine_old_parent as parent

INTERNAL = (
    'drawTowerStrips',
    'clearBoundTarget',
    'CtrRenderer_beginFrame',
)


# DELTARUNE New (flowergpu): tower face retention wraps the clear, the tower draw and flushLazy; cut 2 (rows
# nobody samples) also drawSurfaceImpl; cut 5b (vertex ring) reserveBatch, to tell frames apart at their first draw;
# cut 4b (plain layer copies in VRAM) yieldMirrors and reclaimVram, which give the copies up first.
INTERNAL_NEW = (
    'drawTowerStrips',
    'clearBoundTarget',
    'flushLazy',
    'drawSurfaceImpl',
    'reserveBatch',
    'yieldMirrors',
    'reclaimVram',
)


# Cut 3 (shared depth texture): drawTowerStrips' own two flushBatch calls only, so the second (the strips) can be
# drawn from the shared texture; every other flushBatch call is left as it is.
SCOPED_NEW = (
    ('drawTowerStrips', 'flushBatch', 'Vine_tower_flushBatch', 2),
    # Cut 5b (vertex ring wrap without the GPU wait when it can't matter): makeVertexRoom's waits and its other wrap.
    ('reserveBatch', 'CtrGpu_waitIdle', 'Vine_ring_waitIdle', 2),
    ('reserveBatch', 'endFrameEarly', 'Vine_ring_endFrameEarly', 1),
)


# The VRAM probe (test only) also routes surfaceTexture, when vine_new_retain.inc turns it on.
PROBE_INC = Path(__file__).resolve().parents[2] / 'platform' / '3ds' / 'source' / 'vine_new_retain.inc'


def vram_probe_on():
    return any(line.startswith('#define CTRPORT_FLOWERGPU_VRAMPROBE') for line in PROBE_INC.read_text(encoding='utf-8').splitlines())


def retain_oomport_slots(assembly):
    # Both functions remain in their original contiguous late input section.
    # Checked growth and reset helpers shrink them; keep the original extents,
    # with an assembly error for growth. check_oomport_layout proves that each
    # zero tail follows a terminal barrier and has no incoming branch/address.
    for name, size in (('vineEnsureVramForFaces', 940), ('vineActivateStage2', 2624)):
        pattern = rf'(?m)^([ \t]*\.size[ \t]+{name},[ \t]*\.-{name}[ \t]*)$'
        fence = f'oomport_tail_{name}:\n\t.org {name}+{size}\n'
        assembly, count = re.subn(pattern, lambda m: fence + m[0], assembly)
        assert count == 1, ('missing/duplicate retained tower body', name, count)
    return assembly


def main():
    if '-DENABLE_WAD16' in sys.argv:
        import utwindow_compile
        return utwindow_compile.main(sys.argv[1:])
    internal = INTERNAL_NEW if '-DCTRPORT_TARGET_N3DS' in sys.argv else INTERNAL
    if internal is INTERNAL_NEW and vram_probe_on():
        internal = INTERNAL_NEW + ('surfaceTexture',)
    parent.INTERNAL = internal
    parent.EXTERNAL = ()
    parent.ENTRIES = internal
    parent.SCOPED = SCOPED_NEW if internal is not INTERNAL else ()
    return parent.main(retain_oomport_slots if internal is INTERNAL else None)


if __name__ == '__main__':
    sys.exit(main())
