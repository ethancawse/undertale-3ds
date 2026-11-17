"""DR New pinwheel Create/Draw event wrappers, enabled by build.sh.

Only two cold event bindings change. Their original generated bodies, game
values and event ordering remain intact. No per-frame VM/renderer dispatch
test is added. Inputs/derived code stay in the builder directory.
"""
import hashlib
import os
from pathlib import Path
from data_versions import TABLE,load_table

ENV='CTRPORT_WEIRDSHD_PINWHEEL'
EVENTS=('gml_Object_obj_ch5_LW20W_rotate_Create_0',
        'gml_Object_obj_ch5_LW20W_rotate_Draw_0')
# Pin the public metadata file, then use its measured chapter row. This is a
# SOURCE-file digest, not a second embedded player-data digest. A developer
# test requires deliberate re-pinning; player builds simply leave the effect off.
VERSION_TABLE_SHA256='470a901fada525decafd185d8874716aff74abb8192b03aef04412b39e1cbb4d'


def expected_input():
    try:
        if hashlib.sha256(TABLE.read_bytes()).hexdigest()!=VERSION_TABLE_SHA256:
            return None
        _,known=load_table()
        rows=[(size,digest) for game,part,size,digest in known
              if game=='deltarune' and part=='chapter5_windows']
        return rows[0] if len(rows)==1 else None
    except (OSError,ValueError,AttributeError,TypeError):
        return None


def enabled(game,target):
    return game=='deltarune' and target=='n3ds' and os.environ.get(ENV)=='1'


def validate(dw,target,part=None):
    """Before operand patching, admit only the measured player's CH5 input."""
    dw.weirdshd_pinwheel=False
    if target!='n3ds' or os.environ.get(ENV)!='1':
        return
    if part!='chapter5_windows' and not any(n in dw.code_by_name for n in EVENTS):
        return
    expected=expected_input()
    if (expected is not None and dw.wad==17 and all(n in dw.code_by_name for n in EVENTS)
            and len(dw.data)==expected[0] and hashlib.sha256(dw.data).hexdigest()==expected[1]):
        dw.weirdshd_pinwheel=True
    else:
        print('Pinwheel: chapter 5 data is not the measured revision; the effect stays off')


def selection(lines,game,target,part):
    if not enabled(game,target) or part!='chapter5_windows': return lines
    # Cold events may be outside the profiled selection. Both must bind to
    # wrappers, including when their original weak generated body is omitted.
    present={line.split()[1] for line in lines if len(line.split())>=2 and not line.lstrip().startswith('#')}
    return list(lines)+[f'{part} {name} extra\n' for name in EVENTS if name not in present]


def variant(dw,name,k):
    if not getattr(dw,'weirdshd_pinwheel',False) or name not in EVENTS: return None
    phase=EVENTS.index(name)
    return ([
        'RValue CtrPinwheel_event(VMContext*, AotFn, int32_t);',
        'RValue aotu_%d(VMContext* ctx) { return CtrPinwheel_event(ctx, aotf_%d, %d); }' % (k,k,phase),
    ],'CtrPinwheel_event')


def fingerprint(game,target):
    if not enabled(game,target): return b''
    # A table update must invalidate previously generated admitted wrappers.
    return b'pinwheel event scope v1\0'+Path(__file__).read_bytes()+repr(expected_input()).encode()
