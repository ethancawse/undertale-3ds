"""Read the selected source tree's shared release contract. No copied release values."""
import json
import hashlib
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
LABEL = re.compile(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-(alpha|beta|rc)\.([1-9][0-9]*))?')


def components(value):
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', value):
        raise ValueError('Invalid numeric CIA version: ' + value)
    result = tuple(map(int, value.split('.')))
    if any(n > limit for n, limit in zip(result, (63, 63, 15))):
        raise ValueError('Version exceeds CIA limits (major/minor 0-63, patch 0-15)')
    return result


def packed(parts):
    major, minor, patch = parts
    return major * 1024 + minor * 16 + patch


def check_allocations(table):
    """Every prerelease's CIA allocation, in version order, rises and stays below its own final release, so each
    release's CIA installs over the one before it (docs/release/updates.md; release_contract.cmake checks the same)."""
    rows = []
    for label, value in table.items():
        match = LABEL.fullmatch(label)
        if not match or not match.group(4) or int(match.group(5)) > 9999:
            raise ValueError('CIA allocation %s is not a prerelease version (alpha.N/beta.N/rc.N, N below 10000)'
                             % label)
        final = packed(tuple(int(match.group(i)) for i in (1, 2, 3)))
        cia = packed(components(value))
        if cia >= final:
            raise ValueError('Prerelease CIA allocation must precede its final release (%s: %s)' % (label, value))
        order = (final, ('alpha', 'beta', 'rc').index(match.group(4)), int(match.group(5)))
        rows.append((order, cia, '%s=%s' % (label, value)))
    rows.sort()
    for (_, cia_a, a), (_, cia_b, b) in zip(rows, rows[1:]):
        if cia_a >= cia_b:
            raise ValueError('CIA allocations must rise with the version: %s is not below %s' % (a, b))


def read(root=ROOT):
    root = Path(root)
    with (root / 'docs/release/build-contract.json').open(encoding='utf-8') as source:
        contract = json.load(source)
    if contract.get('schema') != 1:
        raise ValueError('Unsupported release contract schema')
    version_file = contract['version_file']
    if version_file != Path(version_file).name:
        raise ValueError('version_file must be a source-root filename')
    try:
        version = (root / version_file).read_text(encoding='ascii').strip()
    except FileNotFoundError:
        raise ValueError('there is no VERSION file in ' + str(root)) from None
    match = LABEL.fullmatch(version)
    if not match:
        raise ValueError('VERSION says %r, not major.minor.patch with optional alpha.N/beta.N/rc.N' % version[:40])
    prefix = contract['archive_id_prefix']
    limit = contract['build_id_storage_bytes'] - 2
    if not isinstance(prefix, str) or not prefix.isascii() or not prefix.isprintable():
        raise ValueError('Invalid archive ID prefix')
    # Current AOT tags use at most eight ASCII bytes, including their leading +.
    if len(prefix + version + '+aot12ch') > limit:
        raise ValueError('version %s is too long for the console\'s %d-character build ID' % (version, limit))
    numeric = '.'.join(match.group(i) for i in (1, 2, 3))
    final = components(numeric)
    check_allocations(contract.get('cia_prerelease_versions', {}))
    if match.group(4):
        allocation = contract.get('cia_prerelease_versions', {}).get(version)
        if allocation is None:
            raise ValueError('No approved CIA allocation for ' + version)
        cia = components(allocation)
        if packed(cia) >= packed(final):
            raise ValueError('Prerelease CIA allocation must precede its final release')
    else:
        cia = final
    for game in ('undertale', 'deltarune'):
        for model in ('n3ds', 'o3ds'):
            if not re.fullmatch(r'[a-z0-9-]+', contract['package_stem'].format(game=game, model=model)):
                raise ValueError('Unsafe package_stem in release contract')
    return dict(contract, version=version, numeric_version=numeric,
                cia_components=cia, cia_integer=packed(cia))


DESCRIPTOR = 'release-descriptor.json'


def payload_digest(files):
    """Hash sorted POSIX filenames and their byte digests; no archive timestamps."""
    rows = [[name, len(data), hashlib.sha256(data).hexdigest()]
            for name, data in sorted(files.items())]
    return hashlib.sha256(json.dumps(rows, ensure_ascii=True, separators=(',', ':')).encode('ascii')).hexdigest()


def source_descriptor(game, version, digest):
    if game not in ('undertale', 'deltarune'):
        raise ValueError('Unknown release game')
    return dict(schema=1, game=game, version=version, tag='v' + version,
                repository='https://github.com/ethancawse/' + game + '-3ds',
                public_commit=None, source_sha256=digest)


def record_public_source(files, public_tree, game, version, digest, revision='HEAD'):
    """Bind export bytes to an existing local public-export Git commit, never the monorepo HEAD.

    The descriptor is transport metadata outside that commit, avoiding a self-referential commit ID.
    This verifies local corresponding source; publication remains the owner's release step.
    """
    root = Path(public_tree).resolve()
    def git(*args, input=None):
        return subprocess.run(['git', '-C', str(root), *args], input=input,
                              stdin=subprocess.DEVNULL if input is None else None,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              check=True, timeout=30).stdout
    try:
        top = Path(git('rev-parse', '--show-toplevel').decode().strip()).resolve()
        if top != root:
            raise ValueError('Public source must be the root of its own export repository.')
        if revision != 'HEAD' and not re.fullmatch(r'[0-9a-f]{40}', revision):
            raise ValueError('Public revision must be HEAD or a full commit ID.')
        commit = git('rev-parse', '--verify', revision + '^{commit}').decode().strip()
        entries = git('ls-tree', '-r', '-z', '--full-tree', commit).split(b'\0')
        blobs = {}
        for entry in filter(None, entries):
            header, name = entry.split(b'\t', 1)
            # Repository administration is not part of the builder's source payload.
            if name.startswith(b'.github/'):
                continue
            mode, kind, oid = header.split()
            if mode not in (b'100644', b'100755') or kind != b'blob':
                raise ValueError('Public export contains a nonregular file.')
            blobs[name.decode('utf-8')] = oid.decode('ascii')
        if set(blobs) != set(files):
            raise ValueError('Public commit differs from export: missing=%s extra=%s' %
                             (sorted(set(files) - set(blobs))[:5], sorted(set(blobs) - set(files))[:5]))
        # Compare Git blob IDs, not git archive output (export-subst/export-ignore
        # attributes may change that output). Compute in process, not one Git child per file.
        algorithm = git('rev-parse', '--show-object-format').decode().strip()
        if algorithm != 'sha1':
            raise ValueError('Descriptor schema 1 needs SHA-1 Git commit IDs.')
        # Consult the selected commit, not mutable working-tree attributes. Only
        # explicitly CRLF text may differ in line endings; all other bytes stay exact.
        attributes = git('check-attr', '--source=' + commit, '-z', '--stdin', 'eol',
                         input=b''.join(name.encode('utf-8') + b'\0' for name in blobs)).split(b'\0')
        crlf = {attributes[i].decode('utf-8') for i in range(0, len(attributes) - 1, 3)
                if attributes[i + 2] == b'crlf'}
        for name, oid in blobs.items():
            data = files[name]
            actual = hashlib.new(algorithm, b'blob ' + str(len(data)).encode('ascii') + b'\0' + data,
                                 usedforsecurity=False).hexdigest()
            if actual != oid:
                if name in crlf and data.replace(b'\r\n', b'\n') == \
                        git('cat-file', 'blob', oid).replace(b'\r\n', b'\n'):
                    continue
                raise ValueError('Public commit differs from export: changed=' + name)
    except (OSError, subprocess.SubprocessError) as exc:
        raise ValueError('Cannot read the public export commit.') from exc
    if DESCRIPTOR in files:
        raise ValueError('The release descriptor must stay outside the public source commit.')
    descriptor = source_descriptor(game, version, digest)
    descriptor['public_commit'] = commit
    validate_source(descriptor, game, version, digest)
    return descriptor


def validate_source(descriptor, game, version, digest):
    expected = source_descriptor(game, version, digest)
    if not isinstance(descriptor, dict) or any(descriptor.get(k) != v for k, v in expected.items()
                                              if k != 'public_commit'):
        raise ValueError('The release descriptor does not match this source. Download the complete builder again.')
    commit = descriptor.get('public_commit')
    if not isinstance(commit, str) or not re.fullmatch(r'[0-9a-f]{40}', commit) or commit == '0' * 40:
        raise ValueError('This builder has no approved public source link yet. Use a published release builder.')
    return dict(descriptor, source_url=expected['repository'] + '/tree/' + commit)


def build_environment(game, public_commit, env=None):
    """Use the builder's Legal-page profile in candidate builds too.

    The commit URL is validated by CMake; the displayed release tag and heading
    then have identical bytes in both builds. This does not attest publication
    or ownership: release-day approval still binds the public export and tag.
    """
    import os
    if game not in ('undertale', 'deltarune'):
        raise ValueError('Unknown release game')
    if not isinstance(public_commit, str) or not re.fullmatch('[0-9a-f]{40}', public_commit) or public_commit == '0' * 40:
        raise ValueError('Use the full corresponding-source commit')
    result = dict(os.environ if env is None else env)
    result['CTRPORT_RELEASE_SOURCE_URL'] = ('https://github.com/ethancawse/' + game + '-3ds/tree/' + public_commit)
    return result


def candidate_main(argv=None):
    """Run the normal build command with the same Legal metadata as a release kit.

    Example: python3 -I tools/release/release_contract.py --game undertale
      --public-commit <40 hex> -- bash platform/3ds/build.sh undertale o3ds ...
    Apply the same release export, game-derived assets, flags and compiler to
    both arms. A development-profile binary is not a release identity baseline.
    """
    import argparse
    p = argparse.ArgumentParser(description='Candidate build with the builder Legal-page profile')
    p.add_argument('--game', choices=('undertale', 'deltarune'), required=True)
    p.add_argument('--public-commit', required=True)
    p.add_argument('--timeout', type=int, default=14400)
    p.add_argument('command', nargs=argparse.REMAINDER)
    o = p.parse_args(argv)
    command = o.command[1:] if o.command[:1] == ['--'] else o.command
    if not command or not 1 <= o.timeout <= 86400:
        p.error('Supply a build command after -- and a timeout from 1 to 86400 seconds')
    try:
        env = build_environment(o.game, o.public_commit)
    except ValueError as exc:
        p.error(str(exc))
    # Never inherit interactive input into a release build.
    return subprocess.run(command, env=env, stdin=subprocess.DEVNULL, timeout=o.timeout).returncode


if __name__ == '__main__':
    raise SystemExit(candidate_main())
