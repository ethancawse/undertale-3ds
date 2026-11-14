"""One fixed-size writer instruction edit, plus an opt-in late M adapter.

The existing walk still performs complete read-only admission before its run.
Only a verified unsupported-escape dispatch enters the adapter: other reject
edges (bad numeric bounds, bad face globals, percent) do not reach it. Final
ELF proof must pin the old word and all other existing function bytes.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

MARKER = '--mexdlg-native-compiler'
WRITER_SHA256 = '2b9d7709c6d32e0585694891a8103dc919f4859890a3df09b15c24e3e0354db3'
DV8U6_WRITER_SHA256 = 'e0016f1cc126275a80510fef97f0cf3530b895e8f6bd86dc229e53cc6e489426'
DV8U19_D4_WRITER_SHA256 = 'ad32a699499e59dedfe8f9cfee5668f10241454dd8da30a017a3732634ff1548'
# D4 with its comments cleaned for the published source: code and line numbers unchanged, so its walk compiles to
# D4's reviewed walk digest. The export cleanup leaves these bytes as they are (tools/release/export_pins.py).
D4_CLEAN_WRITER_SHA256 = '2dde8b5b0d6632ec70508279b853da34282debfa33cced4316eca76ac6ea90b2'
D12_WRITERS = (DV8U6_WRITER_SHA256, DV8U19_D4_WRITER_SHA256, D4_CLEAN_WRITER_SHA256)
# Exact pre-patch wfWalk scopes from the reviewed Old compiler listings.
# A future source pin also needs its independently qualified walk digest:
# the helper assumes fixed frame slots and register roles, beyond the seam.
WRITER_WALK_SHA256 = {
    WRITER_SHA256: 'f10114740844d9f4016212c2d480984c62e4ccaba0c69f165415975d3c01a0c0',
    DV8U6_WRITER_SHA256: 'df23d3abd7bd60eadc47b98aad10c23d2f62f1c876dbc7b182c837125c567165',
    DV8U19_D4_WRITER_SHA256: '914babd2688f90f881e3bf4659db905156eff4371f6c6b80cf29f81ee1c8db73',
    D4_CLEAN_WRITER_SHA256: '914babd2688f90f881e3bf4659db905156eff4371f6c6b80cf29f81ee1c8db73',
}
HELPER = 'Mexdlg_nativeM'


def check_walk_assembly(walk, writer_sha256):
    expected = WRITER_WALK_SHA256.get(writer_sha256)
    assert expected is not None, 'Unpinned writer walk source: ' + writer_sha256
    actual = hashlib.sha256(walk.encode()).hexdigest()
    assert actual == expected, ('Unpinned writer walk assembly for ' + writer_sha256 +
        ': expected ' + expected + ', got ' + actual)


def helper_assembly(normal, reject, mask):
    return f'''
 .pushsection .text_mexdlg,"ax",%progbits
 .balign 4
 .arm
 .global {HELPER}
 .type {HELPER}, %function
{HELPER}:
 cmp r4, #77
 bne .Lmexdlg_other
 push {{r0-r12, lr}}
 vmrs r0, FPSCR
 push {{r0, ip}}
 vpush {{d0-d15}}
 mov r0, r9
 ldr r1, [sp, #312]
 add r2, r8, #2
 bl wfAt
 sub r1, r0, #48
 cmp r1, #9
 bhi .Lmexdlg_refuse
 mov r1, r0
 ldr r0, [sp, #208]
 mov r2, r5
 bl Mexdlg_flag
 cmp r0, #0
 beq .Lmexdlg_refuse
 mov r0, r7
 mov r1, r7
 ldr r2, .Lmexdlg_one
 mov r3, #0
 bl wfOp
 vpop {{d0-d15}}
 pop {{r0, ip}}
 vmsr FPSCR, r0
 pop {{r0-r12, lr}}
 vldr d8, [sp, #24]
 b {normal}
.Lmexdlg_refuse:
 vpop {{d0-d15}}
 pop {{r0, ip}}
 vmsr FPSCR, r0
 pop {{r0-r12, lr}}
 b {reject}
.Lmexdlg_other:
 ldr r3, .Lmexdlg_mask
 bx lr
 .balign 4
.Lmexdlg_one:
 .word wfK+80
.Lmexdlg_mask:
 .word {mask}
 .size {HELPER}, .-{HELPER}
 .popsection
 .global Mexdlg_writerCode
 .set Mexdlg_writerCode, wfCode
 .global Mexdlg_writerCall
 .set Mexdlg_writerCall, wfCall
'''


def route(text, *, writer_sha256=WRITER_SHA256):
    assert writer_sha256 == WRITER_SHA256, 'Wrong source for plain writer route'
    scopes = list(re.finditer(r'(?ms)^wfWalk\.constprop\.0:\n.*?^\s*\.size\s+wfWalk\.constprop\.0,\s*\.-wfWalk\.constprop\.0[^\n]*\n', text))
    assert len(scopes) == 1, 'Expected the dv8q plain writer walk'
    scope = scopes[0]
    old = scope[0]
    check_walk_assembly(old, writer_sha256)
    pattern = re.compile(r'(?m)^(\tldr\tr3, ([^\n]+))\n\ttst\tip, r3\n\tbeq\t(\.L\d+)\n(\.L\d+):\n\tmov\tr5, #0\n')
    sites = list(pattern.finditer(old))
    assert len(sites) == 1, 'Unknown unsupported-escape dispatch'
    site = sites[0]
    # The current source/flags and linked gate pin every instruction. This
    # literal resolves the original mask; no condition or other edge changes.
    literal = re.fullmatch(r'(\.L\d+)\+(\d+)', site[2])
    assert literal, site[2]
    pool = old[old.index('\n' + literal[1] + ':\n'):]
    words = re.findall(r'^\s*\.word\s+([^\n]+)', pool, re.M)
    mask = words[int(literal[2]) // 4]
    assert int(mask, 0) == 0x8021, mask
    lr_lines = [line.strip() for line in old.splitlines() if re.search(r'\blr\b', line)]
    assert len(lr_lines) == 4 and lr_lines[:2] == [
        'push\t{r4, r5, r6, r7, r8, r9, r10, fp, lr}', 'mov\tlr, #1'], lr_lines
    assert re.fullmatch(r'ands\tr0, r0, lr, (?:asl|lsl) r1', lr_lines[2]), lr_lines
    assert lr_lines[3] in ('mov\tip, lr, asl r1', 'lsl\tip, lr, r1'), lr_lines
    changed = old[:site.start()] + '\tbl\t' + HELPER + old[site.start() + len(site[1]):]
    assert changed.replace('\tbl\t' + HELPER, site[1]) == old
    result = text[:scope.start()] + changed + text[scope.end():]
    result += helper_assembly(site[3], site[4], mask)
    return result, dict(original_walk_assembly_sha256=hashlib.sha256(old.encode()).hexdigest(),
        normal=site[3], reject=site[4], mask=mask, modified_existing_instructions=1,
        adapter_spill_bytes=192, persistent_state_bytes=0, heap_bytes=0)


def d12_helper_assembly(normal, reject, mask):
    # D1+D2's allocation differs from the archived plain-writer profile.
    # Keep that profile intact for the offline and historical proofs.
    text = helper_assembly(normal, reject, mask)
    registers = {'r4': 'r8', 'r5': 'r6', 'r7': 'r5', 'r8': 'r7'}
    text = re.sub(r'\br[0-9]+\b', lambda m: registers.get(m[0], m[0]), text)
    assert text.count('ldr r0, [sp, #208]') == text.count('mov r2, r6') == 1
    text = text.replace('ldr r0, [sp, #208]', 'ldr r0, [sp, #212]')
    # The walk carries a WfRun pointer; the checked store takes an ABI bool.
    return text.replace('mov r2, r6', 'cmp r6, #0\n movne r2, #1\n moveq r2, #0')


def d12_route(text, *, writer_sha256):
    assert writer_sha256 in D12_WRITERS, 'Wrong source for D1+D2 writer route'
    scopes = list(re.finditer(r'(?ms)^wfWalk\.constprop\.0:\n.*?^\s*\.size\s+wfWalk\.constprop\.0,\s*\.-wfWalk\.constprop\.0[^\n]*\n', text))
    assert len(scopes) == 1, 'Expected exact D1+D2 walk'
    scope = scopes[0]
    old = scope[0]
    check_walk_assembly(old, writer_sha256)
    pattern = re.compile(r'(?m)^(\tldr\tr3, ([^\n]+))\n\ttst\tr0, r3\n\tbeq\t(\.L\d+)\n(\.L\d+):\n\tmov\tr8, #0\n')
    sites = list(pattern.finditer(old))
    assert len(sites) == 1, 'Unknown D1+D2 unsupported-escape dispatch'
    site = sites[0]
    literal = re.fullmatch(r'(\.L\d+)\+(\d+)', site[2])
    assert literal, site[2]
    pool = old[old.index('\n' + literal[1] + ':\n'):]
    words = re.findall(r'^\s*\.word\s+([^\n]+)', pool, re.M)
    mask = words[int(literal[2]) // 4]
    assert int(mask, 0) == 0x8021, mask
    lr_lines = [line.strip() for line in old.splitlines() if re.search(r'\blr\b', line)]
    assert lr_lines == ['push\t{r4, r5, r6, r7, r8, r9, r10, fp, lr}'], lr_lines
    changed = old[:site.start()] + '\tbl\t' + HELPER + old[site.start() + len(site[1]):]
    assert changed.replace('\tbl\t' + HELPER, site[1]) == old
    return text[:scope.start()] + changed + text[scope.end():] + d12_helper_assembly(site[3], site[4], mask), dict(
        original_walk_assembly_sha256=hashlib.sha256(old.encode()).hexdigest(),
        normal=site[3], reject=site[4], mask=mask, modified_existing_instructions=1,
        adapter_spill_bytes=192, persistent_state_bytes=0, heap_bytes=0, d1d2=True)


def writer_route(source):
    digest = hashlib.sha256(Path(source).read_bytes()).hexdigest()
    if digest in D12_WRITERS:
        return d12_route
    assert digest == WRITER_SHA256, 'Unpinned writer source'
    return route


def run(argv):
    return subprocess.run(argv, stdin=subprocess.DEVNULL, timeout=1800).returncode


def main(args):
    index = args.index(MARKER)
    delegate, command = [a for a in args[:index] if a], args[index + 1:]
    names = ('z_writerfront.c', 'z_mexdlg_native.c')
    sources = [a for a in command if Path(a.replace('\\', '/')).name in names]
    if len(sources) != 1 or '-DCTRPORT_TARGET_O3DS' not in command or '-DENABLE_WAD16' not in command:
        return run(delegate + command)
    assert '-c' in command and command.count('-o') == 1
    source = sources[0]
    output = Path(command[command.index('-o') + 1])
    writer = Path(source.replace('\\', '/')).name == 'z_writerfront.c'
    if writer:
        selected_route = writer_route(source)
        writer_sha256 = hashlib.sha256(Path(source).read_bytes()).hexdigest()
        assert '-Os' in command and '-fno-tree-switch-conversion' in command and '-fno-jump-tables' in command
    original = output.with_name(output.stem + '_mexdlg0.s')
    emit = list(command)
    emit[emit.index('-c')] = '-S'
    emit[emit.index('-o') + 1] = str(original)
    # Debug directives do not affect machine code; omit them to keep the
    # instruction-pattern guard independent of source-file path spellings.
    if run(delegate + emit + ['-g0']):
        return 1
    text = original.read_text(encoding='utf-8')
    if writer:
        text, receipt = selected_route(text, writer_sha256=writer_sha256)
        receipt['writer_source_sha256'] = writer_sha256
    else:
        # This new object has no old placement. Keep even a compiler-created
        # literal or outlined inline helper out of warm text and constants.
        text = re.sub(r'(?m)^(\s*\.section\s+)\.rodata[\w.]*', r'\1.rodata_mexdlg', text)
        text = re.sub(r'(?m)^(\s*\.section\s+)\.text[\w.]*', r'\1.text_mexdlg', text)
        text = re.sub(r'(?m)^\s*\.text\s*$', '\n .section .text_mexdlg,"ax",%progbits', text)
        receipt = dict(new_object=True, heap_bytes=0)
    patched = output.with_name(output.stem + '_mexdlg1.s')
    with patched.open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(text)
    assemble, n = [command[0]], 1
    while n < len(command):
        arg = command[n]
        if arg in ('-MF', '-MT', '-MQ', '-o'):
            n += 2
            continue
        if arg not in ('-MD', '-MMD', '-MP', '-c') and arg != source:
            assemble.append(arg)
        n += 1
    status = run(assemble + ['-Wa,--fatal-warnings', '-x', 'assembler', '-c', str(patched), '-o', str(output)])
    receipt.update(exit=status, source=source, patched_assembly_sha256=hashlib.sha256(text.encode()).hexdigest())
    output.with_name(output.stem + '_mexdlg.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
