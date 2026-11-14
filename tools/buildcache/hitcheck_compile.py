"""Route qualified UT box readers without changing GCC's original graph.

The existing colexact/RNG adapters may request -S instead of -c. Compose before
their canonical pool/retained-slot passes. Unexpected callers, counts, or
IPA-SRA parameter types fail closed; the internal legacy fallback stays raw.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

MARKER = '--hitcheck-compiler'
# These two late bodies shrink when their inlined bbox becomes a corrected
# call. Keep their original slots on both UT models; later code/data must stay.
GM14_SLOTS = {'Colflip14_pointHits': (2968, 'Hitcheck_pointBodyEnd'),
              'Colflip14_rectHitsInstance': (4220, 'Hitcheck_rectBodyEnd')}
READER_SLOTS = {
    'z_utflames.c': {'utfRect': (2080, 'Hitcheck_utfRectBodyEnd'),
                     'utfBox': (1612, 'Hitcheck_utfBoxBodyEnd'),
                     # These callers also shrink after GCC sees the routed
                     # callee's register clobbers. Preserve their pools too.
                     'utfCircle': (1408, 'Hitcheck_utfCircleBodyEnd'),
                     'utfRectFast.constprop.0': (2244, 'Hitcheck_utfRectFastBodyEnd')},
    'colline_gm14.c': {'colline14FirstHit': (472, 'Hitcheck_lineFirstBodyEnd'),
                       'builtin_collision_circle_gm14': (244, 'Hitcheck_circleBodyEnd'),
                       'Colline14_distanceToPoint': (168, 'Hitcheck_distancePointBodyEnd')},
}
RUNNER_BBOX = {'dispatchCollisionEvents': 4, 'stepFrame': 1}
VM_BBOX = {
    'Collision_circleOverlapsInstance': 1, 'Collision_ellipseOverlapsInstance': 2,
    'builtin_collision_point': 1, 'placeEmptyAt': 2, 'placeFreeAt': 2,
    'builtin_place_free': 2, 'mpTestFree': 2, 'builtin_instance_place': 2,
    'builtin_instance_position': 1, 'builtin_collision_line': 2,
    'builtin_position_meeting': 1, 'builtin_collision_rectangle': 2,
    'builtin_place_meeting': 4, 'builtin_collision_circle': 1,
    'builtin_instance_place_list': 2, 'builtin_collision_ellipse': 1,
    'builtin_collision_circle_list': 1, 'builtin_collision_rectangle_list': 2,
    'builtin_collision_line_list': 1, 'builtin_collision_ellipse_list': 1,
}
VM_PAIR = {'placeEmptyAt': 1, 'placeFreeAt': 1, 'builtin_place_free': 1,
    'mpTestFree': 1, 'builtin_instance_place': 1, 'builtin_place_meeting': 3,
    'builtin_instance_place_list': 1}
VM_READERS = {'builtin_distance_to_point': 1,
    'builtin_distance_to_object': 2, 'VMBuiltins_getVariable': 4, 'cdBoxSide': 1}
VM_BBOX.update(VM_READERS)
# UT data does not reference region builtins (hitcheck_guard.py). Leave their
# original generic bodies and registration pointers intact for the shared VM.
VM_UNCHANGED = {'builtin_instance_activate_region': 1,
                'builtin_instance_deactivate_region': 1}


def qualify_pair_abi(dump):
    signatures = re.findall(r'^(?:bool|_Bool) Collision_instancesOverlapPrecise\.isra(?:\.\d+)?\s*\(([^\n]+)\)', dump, re.M)
    assert len(signatures) == 1, ('missing/duplicate pair IPA signature', signatures)
    params = [re.sub(r'\s+[A-Za-z_][\w.]*$', '', x.strip()) for x in signatures[0].split(',')]
    # GCC may spell C's boolean type as bool or _Bool in an optimized dump.
    params = [p.replace('_Bool', 'bool').replace('double', 'GMLReal') for p in params]
    assert params == ['struct DataWin *', 'bool', 'struct Instance *', 'struct Instance *'] + ['GMLReal'] * 8, (
        'pair clone ABI changed; qualify registers before redirecting', params)
    return signatures[0]


def route_runner_free(text):
    """Redirect only Runner_free's unique unconditional terminal free branch."""
    body = re.search(r'(?ms)^Runner_free:\n.*?^\s*\.size\s+Runner_free,\s*\.-Runner_free\s*$', text)
    assert body, 'missing Runner_free body'
    calls = list(re.finditer(r'(?m)^\s*b\s+(free)(?:\(PLT\))?\s*$', body[0]))
    assert len(calls) == 1, ('missing/duplicate terminal runner free', len(calls))
    call = calls[0]
    prefix = body[0][:call.start()]
    # Metadata may intervene after the epilogue, but no ARM instruction may.
    instructions = [line.strip() for line in prefix.splitlines()
                    if re.match(r'^\s+[a-z][a-z0-9.]*\s', line)]
    assert instructions and re.fullmatch(r'pop\s+\{[^}]*\blr\}', instructions[-1]), instructions[-3:]
    start, end = body.start()+call.start(1), body.start()+call.end(1)
    return text[:start]+'Hitcheck_freeRunner'+text[end:]


def rewrite(text, source):
    before = text
    if source == 'collision_gm14.c' or source in READER_SLOTS:
        slots = GM14_SLOTS if source == 'collision_gm14.c' else READER_SLOTS[source]
        for name, (capacity, end) in slots.items():
            pattern = r'(?m)^(\s*\.size\s+' + re.escape(name) + r',\s*\.-' + re.escape(name) + r'\s*)$'
            assert len(re.findall(pattern, text)) == 1, ('missing/duplicate late body', name)
            assert end not in text, ('duplicate body marker', end)
            text = re.sub(pattern, lambda m: '\n.global '+end+'\n'+end+':\n'
                          ' .space '+str(capacity)+'-(.-'+name+'), 0\n'+m[1], text)
        return text, dict(slots=slots, before_sha256=hashlib.sha256(before.encode()).hexdigest(),
                          after_sha256=hashlib.sha256(text.encode()).hexdigest())
    if source == 'runner.c':
        wanted = {'Collision_computeBBox': RUNNER_BBOX,
                  'Collision_instancesOverlapPrecise.isra.0': {'dispatchCollisionEvents': 2}}
        untouched = {'Collision_computeBBox': {'Colexact_sharedBBox': 1}}
    elif source == 'vm_builtins.c':
        wanted = {'Collision_computeBBox': VM_BBOX, 'Collision_instancesOverlapPrecise.isra.0': VM_PAIR}
        untouched = {'Collision_computeBBox': VM_UNCHANGED}
    else:
        assert source == 'spatial_grid.c', source
        wanted = {'Colexact_sharedBBox': {'SpatialGrid_syncGrid': 1}}
        untouched = {}
    targets = {'Collision_computeBBox': 'Hitcheck_collisionBBox',
               'Colexact_sharedBBox': 'Hitcheck_collisionBBox',
               'Collision_instancesOverlapPrecise.isra.0': 'Hitcheck_pair'}
    counts, kept, patches = {}, {}, []
    # .type names identify function ownership even when GCC emits public aliases.
    types = set(re.findall(r'^\s*\.type\s+([\w.]+),\s*%function', text, re.M))
    caller = None
    offset = 0
    for line in text.splitlines(True):
        label = re.match(r'^([\w.]+):', line)
        if label and label[1] in types:
            caller = label[1]
        call = re.match(r'^(\s*b(?:l)?(?:eq|ne|lt|ge|gt|le|hi|ls|cs|cc)?\s+)([\w.]+)(\(PLT\))?([ \t]*(?:@[^\n]*)?\n?)$', line)
        if call and call[2] in wanted:
            old = call[2]
            table = counts if caller in wanted[old] else kept
            table.setdefault(old, {})[caller] = table.setdefault(old, {}).get(caller, 0) + 1
            if table is counts:
                patches.append((offset+call.start(2), offset+call.end(2), targets[old]))
        offset += len(line)
    assert counts == wanted, ('collision call inventory changed', source, counts, wanted)
    assert kept == untouched, ('non-collision bbox inventory changed', source, kept, untouched)
    for start, end, replacement in reversed(patches):
        text = text[:start] + replacement + text[end:]
    if source == 'runner.c':
        text = route_runner_free(text)
    # Prove that every change is exactly one qualified call operand.
    assert len(patches) == sum(sum(v.values()) for v in wanted.values())
    retention = None
    if source != 'spatial_grid.c':
        original_pair = 'Collision_instancesOverlapPrecise.isra.0'
        assert original_pair in types, 'missing original pair section to retain'
        retention = 'Hitcheck_retain' + ('Runner' if source == 'runner.c' else 'VM') + 'Pair'
        assert retention not in text, 'duplicate warm retention entry'
        # Redirecting every call must not let --gc-sections drop the old warm
        # body or its transitive helpers. KEEP only this unreachable late branch.
        text += ('\n.pushsection .text.late.hitcheck,"ax",%progbits\n'
                 '.balign 4\n.global '+retention+'\n.type '+retention+', %function\n'
                 +retention+':\n b '+original_pair+'\n'
                 +'.size '+retention+', .-'+retention+'\n.popsection\n')
    return text, dict(calls=counts, unchanged_calls=kept, retention=retention,
        before_sha256=hashlib.sha256(before.encode()).hexdigest(),
        after_sha256=hashlib.sha256(text.encode()).hexdigest())


def run(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL, timeout=1800).returncode


def main(args):
    split = args.index(MARKER)
    delegate, command = [a for a in args[:split] if a], args[split+1:]
    sources = [a for a in command if Path(a.replace('\\', '/')).name in
               ('runner.c', 'vm_builtins.c', 'spatial_grid.c', 'collision_gm14.c', *READER_SLOTS)]
    if len(sources) != 1 or '-DENABLE_WAD16' not in command or '-DUSE_FLOAT_REALS' in command:
        return run(delegate+command)
    assert command.count('-o') == 1 and (('-c' in command) != ('-S' in command))
    source = sources[0]
    name = Path(source.replace('\\', '/')).name
    output = Path(command[command.index('-o')+1])
    tag = {'runner.c':'hcr', 'vm_builtins.c':'hcv', 'spatial_grid.c':'hcg', 'collision_gm14.c':'hcm',
           'z_utflames.c':'hcf', 'colline_gm14.c':'hcl'}[name]
    original, patched, dump = [output.with_name(tag+x) for x in ('0.s', '1.s', '-opt.txt')]
    emit = list(command)
    if '-c' in emit:
        emit[emit.index('-c')] = '-S'
    emit[emit.index('-o')+1] = str(original)
    if name in ('runner.c', 'vm_builtins.c'):
        emit += ['-fdump-tree-optimized='+str(dump)]
    status = run(delegate+emit)
    if status:
        return status
    text, receipt = rewrite(original.read_text(encoding='utf-8'), name)
    if name in ('runner.c', 'vm_builtins.c'):
        receipt['pair_abi'] = qualify_pair_abi(dump.read_text(encoding='utf-8'))
    with patched.open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(text)
    if '-S' in command:
        with output.open('w', encoding='utf-8', newline='\n') as stream:
            stream.write(text)
    else:
        assemble, index = [command[0]], 1
        while index < len(command):
            arg = command[index]
            if arg in ('-MF', '-MT', '-MQ', '-o'):
                index += 2
                continue
            if arg not in ('-MD', '-MMD', '-MP', '-c') and arg != source:
                assemble.append(arg)
            index += 1
        status = run(assemble+['-Wa,--fatal-warnings', '-x', 'assembler', '-c', str(patched), '-o', str(output)])
    receipt.update(source=name, exit=status)
    output.with_name(tag+'.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
    print('hitcheck collision redirects: '+json.dumps(receipt), flush=True)
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
