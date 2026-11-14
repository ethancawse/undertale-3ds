"""Keep UT's shared decoder slots while removing the AAC implementation.

Only CMake-selected Release/RelWithDebInfo Tremor builds use --aacout-pad and
the retained 08b22205a extents. Emit ordinary guarded C, pad each function after its final literal pool, then restore input
section order. Never rewrite an instruction or relocation. The independent
linked verifier must still prove every non-vendor symbol and data byte.
"""
from pathlib import Path
import fnmatch
import json
import re
import shlex
import subprocess
import sys

MARKER = '--aacout-compiler'
PAD = '--aacout-pad'
# All four original UT decoder objects have these identical input extents.
SLOTS = (
    ('tremorClose', 8), ('openSource', 384), ('srcRead', 232), ('srcSeek', 308),
    ('tremorSeek', 24), ('aacDecodeNext', 316), ('oggProbeTail.part.0', 152),
    ('tremorRead', 64), ('oggProbeHead.part.0.constprop.0', 172), ('tremorTell', 80),
    ('CtrDecoder_sectionPath', 108), ('CtrDecoder_vorbisBackendName', 12),
    ('CtrDecoder_sniff', 160), ('CtrDecoder_startReadAhead', 128),
    ('CtrDecoder_read', 532), ('CtrDecoder_seek', 488), ('CtrDecoder_close', 152),
    ('openCommon', 1304), ('CtrDecoder_openFile', 132), ('CtrDecoder_openMemory', 68),
    ('CtrDecoder_probeFile', 872), ('CtrDecoder_probeMemory', 356),
)

def rewrite(text):
    # Function sections include their own debug directives and literal pools.
    chunks = re.split(r'(?=^[ \t]*\.section[ \t]+)', text, flags=re.M)
    functions = {}
    places = []
    for i, chunk in enumerate(chunks):
        match = re.match(r'\s*\.section\s+\.text\.([^,\s]+)', chunk)
        if not match:
            continue
        name = match[1]
        assert name not in functions, ('duplicate decoder function section', name)
        functions[name] = chunk
        places.append(i)
    expected = {name for name, _ in SLOTS}
    assert set(functions) <= expected, ('new decoder function', set(functions) - expected)
    assert expected - set(functions) <= {'aacDecodeNext'}, ('missing decoder function', expected - set(functions))
    result = []
    for name, size in SLOTS:
        if name == 'aacDecodeNext' and name not in functions:
            # A never-called retained wrapper slot; it is not vendor payload.
            chunk = (f'\t.section .text.{name},"ax",%progbits\n\t.align 3\n'
                     f'\t.type {name}, %function\n{name}:\n\t.space {size}, 0\n'
                     f'\t.size {name}, .-{name}\n')
        else:
            chunk = functions[name]
            pattern = r'(?m)^([ \t]*\.size[ \t]+' + re.escape(name) + r',[ \t]*\.-' + re.escape(name) + r')[ \t]*$'
            assert len(re.findall(pattern, chunk)) == 1, ('unqualified decoder size', name)
            # .org resolves after ARM literal-pool alignment; .if cannot use
            # these relaxation-dependent differences. Backwards .org is fatal.
            chunk = re.sub(pattern, lambda m: f'\t.org {name}+{size}, 0\n' + m[1], chunk)
        result.append(chunk)
    assert places
    # Keep non-function sections (notably the canonical callbacks and strings)
    # in their original relative order; only text input sections are reordered.
    first = places[0]
    return ''.join(chunks[:first] + result + [c for i, c in enumerate(chunks[first:], first) if i not in places])

def run(args):
    return subprocess.run(args, stdin=subprocess.DEVNULL, timeout=1800).returncode

def check_selectors(script, command):
    """Require exactly one linker input per tagged application selector (and per dev-only object selector)."""
    def expand(arguments, active=()):
        result = []
        for arg in arguments:
            if arg.startswith('@'):
                path = Path(arg[1:]).resolve()
                if path in active:
                    raise ValueError('recursive linker response file: ' + str(path))
                # Preserve Windows path backslashes while accepting quoted paths.
                words = shlex.split(path.read_text(encoding='utf-8-sig'), posix=False)
                result.extend(expand([w[1:-1] if w.startswith('"') and w.endswith('"') else w
                                      for w in words], active + (path,)))
            else:
                result.append(arg.replace('\\', '/'))
        return result

    text = Path(script).read_text(encoding='utf-8')
    tag = '/* aacout object */'
    selectors = re.findall(re.escape(tag) + r'\s*(?:KEEP\()?([^\s()]+)\((\.text\.\*|\.rodata)\)\)?', text)
    if not selectors or len(selectors) != text.count(tag):
        raise ValueError('missing or malformed AAC object selectors')
    objects = [arg for arg in expand(command) if arg.endswith('.o')]
    for pattern, kind in selectors:
        matches = [obj for obj in objects if fnmatch.fnmatchcase(obj, pattern)]
        if len(matches) != 1:
            raise ValueError('AAC selector %s(%s) matches %d linker objects; expected 1'
                             % (pattern, kind, len(matches)))
    return len(selectors) + check_devonly(text, objects, {pattern for pattern, _ in selectors})


def check_devonly(text, objects, prefix_patterns):
    """Dev builds' dev-only AAC inputs (aacout.cmake, CTRPORT_DEVONLY_AAC): each object selector names exactly one
    linker object, which no application-prefix selector may also take, and every dev-only line sits between its
    kind's dev-only start and end markers. A release script has none of these lines or markers."""
    tag = '/* aacout devonly */'
    count = 0
    for kind in ('text', 'rodata'):
        start = '__aacout_%s_devonly_start = ABSOLUTE(.);' % kind
        end = '__aacout_%s_devonly_end = ABSOLUTE(.);' % kind
        if text.count(start) != text.count(end) or text.count(start) > 1:
            raise ValueError('AAC %s: malformed dev-only markers' % kind)
        if not text.count(start):
            continue
        body = text[text.index(start):text.index(end)]
        lines = [line.strip() for line in body.splitlines() if tag in line]
        if not lines:
            raise ValueError('AAC %s: dev-only markers without inputs' % kind)
        for line in lines:
            match = re.fullmatch(re.escape(tag) + r' INPUT_SECTION_FLAGS \([^)]*\) (\S+?)\(([^()]+)\)', line)
            if not match or match[1].startswith('KEEP'):
                raise ValueError('malformed dev-only AAC selector: ' + line)
            count += 1
            if match[1] == '*':
                continue
            matches = [obj for obj in objects if fnmatch.fnmatchcase(obj, match[1])]
            if len(matches) != 1:
                raise ValueError('dev-only AAC selector %s(%s) matches %d linker objects; expected 1'
                                 % (match[1], match[2], len(matches)))
            if match[1] in prefix_patterns:
                raise ValueError('dev-only AAC object %s is also an application-prefix selector' % match[1])
    if text.count(tag) != count:
        raise ValueError('dev-only AAC selector outside its markers')
    return count


def main(args):
    if args and args[0] == '--aacout-link':
        if len(args) < 4 or args[2] != '--':
            raise ValueError('expected --aacout-link <script> -- <link command>')
        try:
            count = check_selectors(args[1], args[3:])
        except (ValueError, OSError) as error:
            print('aacout: ' + str(error), file=sys.stderr)
            return 2
        print('aacout: %d object selectors match linker inputs' % count, flush=True)
        return run(args[3:])
    split = args.index(MARKER)
    padding = PAD in args[:split]
    delegate, command = [a for a in args[:split] if a and a != PAD], args[split + 1:]
    sources = [a for a in command if a.replace('\\', '/').rsplit('/', 1)[-1] == 'ctr_audio_decoder.c']
    if not padding or not sources:
        return run(delegate + command)
    missing = sorted({'-DCTRPORT_NO_AAC', '-DNDEBUG', '-DCTRPORT_HAVE_TREMOR'} - set(command))
    if missing:
        print('aacout: retained decoder padding requires ' + ', '.join(missing), file=sys.stderr)
        return 2
    assert len(sources) == 1 and '-c' in command and command.count('-o') == 1
    source = sources[0]
    output = Path(command[command.index('-o') + 1])
    original = output.with_name('aacout0.s')
    patched = output.with_name('aacout1.s')
    emit = list(command)
    emit[emit.index('-c')] = '-S'
    emit[emit.index('-o') + 1] = str(original)
    status = run(delegate + emit)
    if status:
        return status
    patched.write_bytes(rewrite(original.read_text(encoding='utf-8')).encode('utf-8'))
    assemble = [command[0]]
    index = 1
    while index < len(command):
        arg = command[index]
        if arg in ('-MF', '-MT', '-MQ', '-o'):
            index += 2
            continue
        if arg not in ('-MD', '-MMD', '-MP', '-c') and arg != source:
            assemble.append(arg)
        index += 1
    status = run(assemble + ['-x', 'assembler', '-c', str(patched), '-o', str(output)])
    output.with_name('aacout.json').write_text(json.dumps(dict(exit=status, retained_slots=dict(SLOTS)), indent=2) + '\n')
    return status

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
