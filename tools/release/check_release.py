#!/usr/bin/env python3
"""Assemble a candidate source ZIP in scratch, then inspect its actual members.

Run from any directory: py -3 tools/release/check_release.py --export-tree
Exit 0: no configured content hits (NOT legal/build clearance); 1: content hits;
2: invalid manifest, unsafe input/output, or unreadable archive. Nothing is published.
Requires Python 3.10+. Only its standard library and local Git are used.
No game files are opened. Run from a Git checkout, not an exported source ZIP.
Exported build.sh requires Python 3.8+ and fails before building when missing;
CTRPORT_PYTHON selects the builder's interpreter through a login-shell PATH reset.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import import_closure
import generate_notices
import release_contract
MAX_MEMBER = 16 * 1024 * 1024
MAX_TOTAL = 128 * 1024 * 1024
LARGE_BINARY = 64 * 1024
LARGE_TEXT = 2 * 1024 * 1024
# Identifiers, not game titles: titles alone are not content provenance evidence.
NAME_RE = re.compile(r'\b(?:gml_(?:Script|Object|Room|GlobalScript)_[A-Za-z0-9_]+|'
                     r'(?:obj|spr|snd|mus|room|scr|shd|sh)_[A-Za-z0-9_]+|textdata_(?:en|ja))\b', re.I)
BAD_EXT = {'.win', '.ogg', '.mp3', '.mp4', '.aac', '.ttf', '.otf', '.cia',
           '.3dsx', '.elf', '.a', '.o', '.dll', '.exe', '.bcstm', '.qoi', '.dsp'}
BAD_BASE = {'credits.txt', 'dspfirm.cdc', 'file0', 'file9', 'undertale.ini'}
BAD_PARTS = {'build', 'cache', 'cache_exact', 'saves', 'romfs', '__pycache__', '.git'}
IDENTIFIER_CLASSES = {
    'aot-selection': {'flagged-game-names'},
    'dialogue-objects': {'flagged-game-names'},
    'data-versions': {'known-data-sha256'},
    'music-versions': {'flagged-game-names', 'known-data-sha256'},
    'engine-lookups': {'flagged-game-names'},
    'engine-symbols': {'flagged-game-names'},
    'synthetic-fixture': {'flagged-game-names'},
    'gating-hashes': {'known-data-sha256'},
    'roomplan-record': {'flagged-game-names', 'known-data-sha256'},
    'roomplan-tool': {'flagged-game-names'},
}
REVIEWED_CLASSES = {'engine-lookups', 'engine-symbols', 'synthetic-fixture',
                    'gating-hashes', 'roomplan-record', 'roomplan-tool'}
FALSE_POSITIVE_CLASSES = {'engine-symbols', 'synthetic-fixture'}
ROOMPLAN_RECORD_RE = re.compile(r'tools/roomplan/records/(?:(?:sounds/)?undertale-(?:steam|gog)-1\.08|'
                               r'deltarune-steam-0\.0\.253-chapter[1-5])\.json|'
                               r'tools/roomplan/phases/deltarune-steam-0\.0\.253-chapter5\.json|'
                               r'tools/roomplan/tracks/deltarune\.json')
SFX_TOOL = 'tools/roomplan/build_sfxplan.py'
ASSET_RULES = {'media-or-output-signature', 'large-binary'}
PRESS_SOURCE = 'platform/3ds/source/ctr_pixfix_press.c'
# The project's old working name stays out of the public tree (relaudit P2, 2026-10-27). Only the old SD folder
# ("3ds/" + it) and settings file (it + ".ini"), which z_sdmigrate.c and z_gamedir.c move players' files from, may
# carry it. Spelt in two parts so that this scanner and its tests are exported without a hit.
WORKING_NAME_RE = re.compile('(?<!3ds/)' + 'delta' + 'tale' + r'(?!\.ini\b)', re.I)


def evidence_sha256(finding):
    """Bind an approval to exact evidence without copying names/hashes into policy."""
    evidence = {key: finding[key] for key in ('rule', 'evidence')}
    return hashlib.sha256(json.dumps(evidence, sort_keys=True,
                                    separators=(',', ':')).encode('utf-8')).hexdigest()


def valid_sha256(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None


def read_identifier_policy(path):
    """D2 and the 2026-10-22 policy permit exact revisions, never directories."""
    policy = json.loads(path.read_text(encoding='utf-8'))
    if policy.get('schema') != 1 or policy.get('decision') != 'D2, owner 2026-10-20':
        raise ValueError('identifier policy needs the approved D2 decision')
    entries = policy.get('entries', [])
    seen = set()
    for entry in entries:
        member, kind = entry['member'], entry['class']
        if (not safe_name(member) or member.casefold() in seen or kind not in IDENTIFIER_CLASSES
                or not valid_sha256(entry['sha256'])):
            raise ValueError('invalid identifier policy entry: ' + repr(entry))
        if not export_path_allowed(member) or any(p in PurePosixPath(member).parts
                                                  for p in ('bench', 'analysis')):
            raise ValueError('private table in identifier policy: ' + member)
        seen.add(member.casefold())
        if kind in REVIEWED_CLASSES:
            evidence = entry.get('evidence_sha256')
            if (not entry.get('justification', '').strip() or not isinstance(evidence, list)
                    or not evidence or not all(valid_sha256(h) for h in evidence)
                    or len(set(evidence)) != len(evidence)):
                raise ValueError('reviewed entry needs justification and exact evidence: ' + member)
        if kind == 'roomplan-record' and not ROOMPLAN_RECORD_RE.fullmatch(member):
            raise ValueError('not an approved roomplan record: ' + member)
        if kind == 'roomplan-tool' and member not in (
                'tools/roomplan/records.py', 'tools/roomplan/build_plan.py', 'tools/roomplan/dwinfo.py',
                'tools/roomplan/build_phases.py', 'tools/roomplan/build_tracks.py', 'tools/roomplan/build_flowcode.py',
                SFX_TOOL):
            raise ValueError('not a roomplan build tool: ' + member)
    return policy


def apply_identifier_policy(hits, members, policy, read_bytes):
    """Keep raw findings; classify only exact approved file revisions separately."""
    revisions = {m['path']: m['sha256'] for m in members}
    entries = {e['member']: e for e in policy['entries']}
    approved, remaining = [], []
    for finding in hits:
        entry = entries.get(finding['member'])
        matches = (entry and finding['rule'] in IDENTIFIER_CLASSES[entry['class']]
                   and revisions[finding['member']] == entry['sha256'])
        if matches and entry['class'] in REVIEWED_CLASSES:
            matches = evidence_sha256(finding) in entry['evidence_sha256']
        elif matches:
            matches = 'names only' in read_bytes(finding['member']).decode('utf-8', errors='strict')[:2048]
        if matches:
            approved.append(dict(finding, identifier_class=entry['class']))
        else:
            remaining.append(finding)
    return remaining, approved


def read_asset_policy(path):
    """Original port media has its own exact-byte provenance decision, not D2."""
    policy = json.loads(path.read_text(encoding='utf-8'))
    if policy.get('schema') != 1 or policy.get('decision') != 'Original port assets, lead 2026-10-22':
        raise ValueError('asset policy needs the approved original-port-asset decision')
    if (policy.get('generator') != 'platform/3ds/assets/make_art.py'
            or not valid_sha256(policy.get('generator_sha256'))
            or hashlib.sha256(safe_local(policy['generator']).read_bytes()).hexdigest()
            != policy['generator_sha256']):
        raise ValueError('original asset generator revision needs review')
    seen = set()
    for entry in policy.get('entries', []):
        member = entry['member']
        if (not (member == PRESS_SOURCE or re.fullmatch(
                r'platform/3ds/assets/(?:(?:banner|icon)-(?:undertale|deltarune)-'
                r'(?:n3ds|o3ds)\.png|banner\.wav)', member))
                or member in seen or not valid_sha256(entry['sha256'])
                or not isinstance(entry.get('bytes'), int) or entry['bytes'] <= 0
                or not entry.get('justification', '').strip()
                or not entry.get('evidence_sha256')
                or not all(valid_sha256(h) for h in entry['evidence_sha256'])):
            raise ValueError('invalid original asset entry: ' + repr(entry))
        seen.add(member)
        if member == PRESS_SOURCE:
            # Original art embedded in source has provenance, but this policy
            # still cannot clear its identifier or any other content finding.
            if (entry.get('approval_date') != '2026-10-22'
                    or entry.get('approved_by') != 'Ethan'
                    or not entry.get('approval_quote', '').strip()
                    or not valid_sha256(entry.get('glyph_rows_sha256'))):
                raise ValueError('PRESS source needs its exact original-art ruling')
            source = safe_local(member).read_bytes()
            if (hashlib.sha256(source).hexdigest(), len(source)) != (entry['sha256'], entry['bytes']):
                raise ValueError('PRESS source revision needs original-art review')
    return policy


def apply_asset_policy(hits, members, policy):
    revisions = {m['path']: m for m in members}
    entries = {e['member']: e for e in policy['entries']}
    approved, remaining = [], []
    for finding in hits:
        entry = entries.get(finding['member'])
        member = revisions[finding['member']]
        if (entry and finding['rule'] in ASSET_RULES
                and (member['sha256'], member['bytes']) == (entry['sha256'], entry['bytes'])
                and evidence_sha256(finding) in entry['evidence_sha256']):
            approved.append(dict(finding, provenance=entry['justification']))
        else:
            remaining.append(finding)
    return remaining, approved


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT, timeout=30,
                                   stdin=subprocess.DEVNULL).decode('utf-8')


def safe_name(name):
    p = PurePosixPath(name)
    return (bool(name) and not p.is_absolute() and p.as_posix() == name
            and all(x not in ('', '.', '..') for x in name.split('/'))
            and not any(c in name for c in '\\:*?[]\x00\r\n'))


def safe_local(name):
    if not safe_name(name):
        raise ValueError('unsafe/nonliteral path: ' + repr(name))
    path = ROOT / name
    for node in (path, *path.parents):
        if node == ROOT:
            break
        if node.is_symlink() or (hasattr(node, 'is_junction') and node.is_junction()):
            raise ValueError('symlink/junction input: ' + name)
    if not path.is_file() or not path.resolve().is_relative_to(ROOT):
        raise ValueError('missing/nonregular/outside input: ' + name)
    return path


def export_path_allowed(name):
    # Never export upstream CI that can retrieve commercial game archives, or
    # non-3DS backends with unresolved licences (B3/S10). This also covers new
    # workflows added upstream after the current exact allow-list was written.
    parts = PurePosixPath(name.lower()).parts
    return '.github' not in parts and 'ps3gl' not in parts


MAP_DIRECTIVE = '#map '
# A file that is a placeholder for the text to publish says so in this marker; the report names it.
PLACEHOLDER_MARK = 'placeholder: replace before publishing'
MD_LINK_RE = re.compile(r'\]\(\s*<?([^)\s>]+)>?(?:\s+"[^"]*")?\s*\)')


def read_manifest_sources(path):
    """The manifest's "#map <member> <- <tracked source>" lines: export members whose bytes come from another file
    (the public README from its template). Other manifest readers skip them as comments."""
    sources = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if not line.startswith(MAP_DIRECTIVE):
            continue
        member, arrow, source = line[len(MAP_DIRECTIVE):].partition(' <- ')
        member, source = member.strip(), source.strip()
        if not arrow or not member or not source or member in sources:
            raise ValueError('bad or repeated manifest map line: ' + line)
        sources[member] = source
    return sources


def read_manifest(path):
    names = [line.strip() for line in path.read_text(encoding='utf-8').splitlines()
             if line.strip() and not line.lstrip().startswith('#')]
    if not names or len({n.casefold() for n in names}) != len(names):
        raise ValueError('manifest is empty or has duplicate/case-colliding paths')
    tracked = {}
    for entry in git('ls-files', '--stage', '-z').split('\0'):
        if entry:
            meta, name = entry.split('\t', 1)
            mode, _, stage = meta.split()
            if stage != '0':
                raise ValueError('unmerged index')
            tracked[name] = mode
    sources = read_manifest_sources(path)
    for member in sources:
        if member not in names:
            raise ValueError('manifest map names a member the manifest lacks: ' + member)
    for name in names:
        source = sources.get(name, name)
        if not export_path_allowed(name) or not export_path_allowed(source):
            raise ValueError('excluded CI/backend in export manifest: ' + name)
        if tracked.get(source) not in ('100644', '100755'):
            raise ValueError('manifest input must be tracked regular file: ' + source)
        safe_local(source)
        # The member's mode is its source's: the archive is built from the source's bytes.
        tracked[name] = tracked[source]
    return names, tracked, sources


def dangling_links(name, data, members):
    """Relative links in an exported Markdown file that point at no exported file."""
    if not name.lower().endswith('.md'):
        return []
    base = PurePosixPath(name).parent
    out = []
    for target in MD_LINK_RE.findall(data.decode('utf-8', errors='replace')):
        target = target.split('#', 1)[0].split('?', 1)[0]
        if not target or re.match(r'[A-Za-z][A-Za-z0-9+.-]*:', target) or target.startswith('/'):
            continue
        parts = []
        for part in (base / target).parts:
            if part == '..':
                if not parts:
                    parts = None
                    break
                parts.pop()
            elif part not in ('.', ''):
                parts.append(part)
        resolved = '/'.join(parts) if parts else None
        if resolved is None or not (resolved in members or any(m.startswith(resolved + '/') for m in members)):
            out.append({'member': name, 'rule': 'dangling-link', 'evidence': target})
    return out


def data_hashes():
    source = safe_local('platform/3ds/data_versions.inc').read_text(encoding='utf-8')
    hashes = re.findall(r'DATA_VERSION\([^\n]*"([0-9a-fA-F]{64})"\)', source)
    if not hashes or len(set(hashes)) != len(hashes):
        raise ValueError('missing/duplicate data-version hashes')
    return [h.lower() for h in hashes]


def inspect_member(name, data, hashes):
    hits = []
    def hit(rule, evidence):
        hits.append({'member': name, 'rule': rule, 'evidence': evidence})
    lower_name = name.lower()
    p = PurePosixPath(lower_name)
    if p.suffix in BAD_EXT or p.name in BAD_BASE or set(p.parts) & BAD_PARTS:
        hit('forbidden-path', name)
    if re.search(r'(?:^|/)aot_(?:gen_[^/]*|table)\.c$', lower_name):
        hit('generated-aot', name)
    if re.search(r'\.(?:zip|gz|bz2|xz|7z|tar)$', lower_name) or data.startswith(
            (b'PK\x03\x04', b'\x1f\x8b', b'BZh', b'\xfd7zXZ\0', b'7z\xbc\xaf\x27\x1c')) or data[257:262] == b'ustar':
        hit('nested-container', 'opaque archive is forbidden; expand and allow-list its sources')
    # Actual GameMaker chunk containers, at any offset, including renamed payloads.
    for m in re.finditer(b'FORM', data):
        i = m.start()
        if data[i+8:i+12] in (b'GEN8', b'AUDO', b'OPTN'):
            hit('game-container', 'FORM/chunk signature at byte %d' % i)
    for signature, label in ((b'OggS\x00', 'Ogg stream'), (bytes.fromhex('664c6143'), 'FLAC stream'),
                             (b'\x89PNG\r\n\x1a\n', 'PNG image'),
                             (b'\x7fELF', 'ELF output'),
                             (b'3DSX\x20\x00', '3DSX output'), (b'3DSX\x2c\x00', '3DSX output')):
        offset = data.find(signature)
        if offset >= 0:
            hit('media-or-output-signature', '%s at byte %d' % (label, offset))
    for m in re.finditer(b'RIFF', data):
        if data[m.start()+8:m.start()+12] == b'WAVE':
            hit('media-or-output-signature', 'WAV stream at byte %d' % m.start())
    # Validate an SFNT directory header so words such as BOTTOM are not font hits.
    for m in re.finditer(b'OTTO|\x00\x01\x00\x00', data):
        i = m.start()
        if i + 12 > len(data):
            continue
        n, search, selector, shift = [int.from_bytes(data[i+j:i+j+2], 'big') for j in (4, 6, 8, 10)]
        if 0 < n <= 256 and i + 12 + 16*n <= len(data):
            power = 1 << (n.bit_length() - 1)
            if (search, selector, shift) == (16*power, power.bit_length()-1, 16*(n-power)):
                hit('media-or-output-signature', 'SFNT font at byte %d' % i)
    # UTF-16 text and raw hash bytes cannot bypass the UTF-8-oriented name scan.
    texts = [data.decode('utf-8', errors='replace')]
    if b'\0' in data:
        texts.extend(data.decode(enc, errors='ignore') for enc in ('utf-16-le', 'utf-16-be'))
    text = '\n'.join(texts)
    for h in hashes:
        if bytes.fromhex(h) in data or h in text.lower():
            hit('known-data-sha256', h)
    digest = hashlib.sha256(data).hexdigest()
    if digest in hashes:
        hit('known-data-file', digest)
    names = sorted(set(NAME_RE.findall(text) + NAME_RE.findall(name)), key=lambda s: (s.casefold(), s))
    if names:
        hit('flagged-game-names', {'count': len(names), 'names': names})
    if WORKING_NAME_RE.search(text) or WORKING_NAME_RE.search(name):
        lines = [i for i, line in enumerate(texts[0].splitlines(), 1) if WORKING_NAME_RE.search(line)]
        hit('internal-name', {'count': len(WORKING_NAME_RE.findall(text)), 'lines': lines[:20]})
    binary = b'\0' in data or '\ufffd' in texts[0]
    if binary and len(data) >= LARGE_BINARY:
        hit('large-binary', '%d bytes >= %d bytes' % (len(data), LARGE_BINARY))
    if len(data) >= LARGE_TEXT:
        hit('large-file', '%d bytes >= %d bytes' % (len(data), LARGE_TEXT))
    # Catch embedded C byte arrays and long base64 strings without storing game output.
    if re.search(r'(?:0x[0-9a-fA-F]{2}\s*,\s*){4096}', text):
        hit('embedded-blob', 'at least 4096 consecutive hex byte literals')
    if re.search(r'[A-Za-z0-9+/]{8192,}={0,2}', text):
        hit('embedded-blob', 'at least 8192 consecutive base64 characters')
    return hits


def scan_archive(path, expected_names, hashes):
    hits, members, total = [], [], 0
    with zipfile.ZipFile(path) as archive:
        infos = archive.infolist()
        names = [i.filename for i in infos]
        if sorted(names) != sorted(expected_names) or len(set(n.casefold() for n in names)) != len(names):
            raise ValueError('assembled ZIP members differ from manifest')
        for info in infos:
            mode = info.external_attr >> 16
            if not safe_name(info.filename) or info.is_dir() or stat.S_ISLNK(mode) or info.flag_bits & 1:
                raise ValueError('unsafe ZIP member: ' + info.filename)
            total += info.file_size
            if info.file_size > MAX_MEMBER or total > MAX_TOTAL:
                raise ValueError('ZIP scan size budget exceeded')
            data = archive.read(info)  # decompresses and checks CRC; inspect these bytes
            members.append({'path': info.filename, 'bytes': len(data),
                            'sha256': hashlib.sha256(data).hexdigest()})
            hits.extend(inspect_member(info.filename, data, hashes))
    return members, hits


def export_and_scan_tree(archive_path, tree, expected_members, hashes):
    """Scan a fresh, expanded export as well as the transport ZIP (S10/S11)."""
    tree.mkdir()  # exclusive: never reuse an old tree or follow an existing link
    with zipfile.ZipFile(archive_path) as archive:
        for member in expected_members:
            name = member['path']
            if not safe_name(name):
                raise ValueError('unsafe export path: ' + name)
            target = tree / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open('xb') as output:
                output.write(archive.read(name))
    members, hits, total = [], [], 0
    for member in expected_members:
        name = member['path']
        data = (tree / name).read_bytes()
        total += len(data)
        if len(data) > MAX_MEMBER or total > MAX_TOTAL:
            raise ValueError('tree scan size budget exceeded')
        actual = {'path': name, 'bytes': len(data),
                  'sha256': hashlib.sha256(data).hexdigest()}
        if actual != member:
            raise ValueError('exported file differs from ZIP: ' + name)
        members.append(actual)
        hits.extend(inspect_member(name, data, hashes))
    return members, hits


def export_product(manifest, game):
    """Select the matching default manifest, or validate an explicitly selected product."""
    if manifest is None:
        game = game or 'undertale'
        return 'tools/release/manifest-' + ('ut' if game == 'undertale' else 'dr') + '.txt', game
    inferred = {'manifest-ut.txt': 'undertale', 'manifest-dr.txt': 'deltarune'}.get(Path(manifest).name)
    if inferred and game and inferred != game:
        raise ValueError('--game ' + game + ' conflicts with ' + manifest)
    game = game or inferred
    if game is None:
        raise ValueError('custom manifest needs --game undertale or --game deltarune')
    return manifest, game


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest',
                        help='repository-relative explicit allow-list; defaults to the selected game (UNDERTALE if omitted)')
    parser.add_argument('--output', default='release-ut-candidate',
                        help='simple basename under scratch (ZIP and JSON)')
    parser.add_argument('--export-tree', action='store_true',
                        help='also expand to a fresh scratch/<output>/ tree and scan its files')
    parser.add_argument('--game', choices=('undertale', 'deltarune'),
                        help='select the default product manifest, or identify a custom manifest (inferred for manifest-ut/dr.txt)')
    parser.add_argument('--public-source', type=Path,
                        help='local repository containing exactly this curated public export commit')
    parser.add_argument('--public-commit', default='HEAD',
                        help='full commit ID in --public-source; default its HEAD, never this monorepo HEAD')
    parser.add_argument('--identifier-policy', default='tools/release/identifier-allowlist.json',
                        help='repository-relative owner-approved D2 table revisions')
    parser.add_argument('--asset-policy', default='tools/release/original-asset-policy.json',
                        help='exact-byte original port media provenance policy')
    args = parser.parse_args()
    try:
        manifest_name, game = export_product(args.manifest, args.game)
        if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.output):
            raise ValueError('output must be a simple basename')
        scratch = ROOT / 'scratch'
        if scratch.is_symlink() or (hasattr(scratch, 'is_junction') and scratch.is_junction()):
            raise ValueError('scratch is a symlink/junction')
        scratch.mkdir(exist_ok=True)
        if not scratch.resolve().is_relative_to(ROOT):
            raise ValueError('scratch is outside worktree')
        archive_path = scratch / (args.output + '.zip')
        report_path = scratch / (args.output + '.json')
        tree = scratch / args.output
        outputs = (archive_path, report_path, tree) if args.export_tree else (archive_path, report_path)
        for path in outputs:
            if path.exists() or path.is_symlink():
                raise ValueError('output already exists; choose another --output: ' + path.name)
        manifest = safe_local(manifest_name)
        names, tracked, sources = read_manifest(manifest)
        closure = import_closure.check(import_closure.Repository(ROOT), set(names), game)
        if closure['missing']:
            raise ValueError('release input closure is incomplete: ' + '; '.join(
                '%s (%s)' % (row['path'], row['required_by']) for row in closure['missing']))
        notices = generate_notices.generate(ROOT, manifest, ROOT / 'tools/release/notices.json')
        if (ROOT / 'THIRD_PARTY_NOTICES').read_text(encoding='utf-8') != notices:
            raise ValueError('THIRD_PARTY_NOTICES is stale for this export manifest.')
        files = {name: safe_local(sources.get(name, name)).read_bytes() for name in names}
        if args.public_source:
            import package_builder
            payload = package_builder.inputs(ROOT, game)
            digest = release_contract.payload_digest({name: payload[name]
                for name in package_builder.pipeline.source_payload(ROOT, game)})
            descriptor = release_contract.record_public_source(files, args.public_source, game,
                release_contract.read(ROOT)['version'], digest, args.public_commit)
            files[release_contract.DESCRIPTOR] = (json.dumps(descriptor, indent=2) + '\n').encode('utf-8')
            names = sorted(files)
        elif args.public_commit != 'HEAD':
            raise ValueError('--public-commit requires --public-source.')
        hashes = data_hashes()
        policy_path = safe_local(args.identifier_policy)
        policy = read_identifier_policy(policy_path)
        asset_policy_path = safe_local(args.asset_policy)
        asset_policy = read_asset_policy(asset_policy_path)
        total = 0
        with zipfile.ZipFile(archive_path, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
            for name in names:
                data = files[name]
                total += len(data)
                if len(data) > MAX_MEMBER or total > MAX_TOTAL:
                    raise ValueError('assembly size budget exceeded')
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.external_attr = (int(tracked.get(name, '100644'), 8) << 16)
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, data)
        members, hits = scan_archive(archive_path, names, hashes)
        if args.export_tree:
            tree_members, tree_hits = export_and_scan_tree(archive_path, tree, members, hashes)
            if (tree_members, tree_hits) != (members, hits):
                raise ValueError('expanded export scan differs from ZIP scan')
        raw_hits = hits
        with zipfile.ZipFile(archive_path) as source:
            hits, allowed = apply_identifier_policy(raw_hits, members, policy, source.read)
        if args.export_tree:
            classified = apply_identifier_policy(tree_hits, tree_members, policy,
                                                  lambda name: (tree / name).read_bytes())
            if classified != (hits, allowed):
                raise ValueError('expanded export identifier policy differs from ZIP')
        identifier_hits = hits
        hits, allowed_assets = apply_asset_policy(identifier_hits, members, asset_policy)
        if args.export_tree and apply_asset_policy(classified[0], tree_members, asset_policy) != (hits, allowed_assets):
            raise ValueError('expanded export asset policy differs from ZIP')
        member_names = set(names)
        placeholders = []
        with zipfile.ZipFile(archive_path) as source:
            for name in names:
                data = source.read(name)
                hits.extend(dangling_links(name, data, member_names))
                if name.lower().endswith('.md') and PLACEHOLDER_MARK.encode() in data:
                    placeholders.append(name)
        false_positives = [h for h in allowed if h['identifier_class'] in FALSE_POSITIVE_CLASSES]
        allowed = [h for h in allowed if h['identifier_class'] not in FALSE_POSITIVE_CLASSES]
        report = {'status': 'FAIL' if hits else 'PASS', 'source_commit': git('rev-parse', 'HEAD').strip(),
                  'build_requirements': {'python': '3.8+', 'missing_python': 'fatal',
                                         'interpreter_override': 'CTRPORT_PYTHON',
                                         'contract': 'docs/release/build-contract.json'},
                  'tracked_changes': git('status', '--porcelain', '--untracked-files=no').splitlines(),
                  'manifest_sha256': hashlib.sha256(manifest.read_bytes()).hexdigest(),
                  'python_input_closure': closure,
                  'archive': archive_path.relative_to(ROOT).as_posix(),
                  'archive_sha256': hashlib.sha256(archive_path.read_bytes()).hexdigest(),
                  'archive_bytes': archive_path.stat().st_size,
                  'payload_bytes': sum(m['bytes'] for m in members),
                  'identifier_policy_sha256': hashlib.sha256(policy_path.read_bytes()).hexdigest(),
                  'identifier_policy_decision': policy['decision'],
                  'asset_policy_sha256': hashlib.sha256(asset_policy_path.read_bytes()).hexdigest(),
                  'asset_policy_decision': asset_policy['decision'],
                  'data_hash_count': len(hashes), 'members': members, 'hits': hits,
                  'raw_hits': raw_hits, 'allowed_identifiers': allowed,
                  'corrected_false_positives': false_positives, 'allowed_original_assets': allowed_assets,
                  'mapped_members': sources, 'placeholder_members': placeholders}
        if args.export_tree:
            report['export_tree'] = tree.relative_to(ROOT).as_posix()
            report['tree_matches_archive'] = True
        report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
        counts = Counter(h['rule'] for h in hits)
        print('%s: %d members, %d payload bytes, %d ZIP bytes, %d hits in %d members' % (
            report['status'], len(members), report['payload_bytes'], report['archive_bytes'],
            len(hits), len({h['member'] for h in hits})))
        for rule, count in sorted(counts.items()):
            print('  %s: %d hits' % (rule, count))
        for kind, count in sorted(Counter(h['identifier_class'] for h in allowed).items()):
            print('  Reviewed identifiers/hashes, %s: %d allowed findings' % (kind, count))
        print('  Reviewed scanner false positives: %d findings' % len(false_positives))
        print('  Original port assets: %d allowed findings' % len(allowed_assets))
        for name, source in sorted(sources.items()):
            print('  %s comes from %s' % (name, source))
        for name in placeholders:
            print('  NOTE: %s is still a placeholder (%s): not ready to publish' % (name, sources.get(name, name)))
        print('Archive (quarantined candidate): ' + report['archive'])
        print('Report: ' + report_path.relative_to(ROOT).as_posix())
        if args.export_tree:
            print('Fresh export tree (identical scan): ' + report['export_tree'])
        return 1 if hits else 0
    except (OSError, ValueError, subprocess.SubprocessError, zipfile.BadZipFile) as exc:
        print('ERROR: ' + str(exc), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
