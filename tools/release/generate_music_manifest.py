"""Generate music metadata: names only, sizes and SHA-256 digests from a read-only Steam or GOG install."""
import argparse
import hashlib
import json
from pathlib import Path

DATA_SHA256 = 'b718f8223a5bb31979ffeed10be6140c857b882fc0d0462b89d6287ae38c81c7'
VERSION = '1.08 (Steam build 4279148)'
MAC_DATA_SHA256 = '91a8acf1f40bd7c45d0bdb978de55187e7fb2cd76a1e2cda41c078dd44d3565e'
MAC_VERSION = '1.08 (Steam macOS)'
GOG_DATA_SHA256 = '946c738c6d6182f683431320a0e32470bc3deb50f3e8887e61998df0c4e3abc8'
GOG_VERSION = '1.08 (GOG)'
MUSIC_COUNT = 212


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def generate(folder, *, store='steam', platform='windows'):
    if (store, platform) not in (('steam', 'windows'), ('steam', 'macos'), ('gog', 'windows')):
        raise ValueError('No verified music profile for this store and platform')
    filename, sha256, version = (('data.win', DATA_SHA256, VERSION) if platform == 'windows'
                                else ('game.ios', MAC_DATA_SHA256, MAC_VERSION))
    if store == 'gog':
        sha256, version = GOG_DATA_SHA256, GOG_VERSION
    if digest(folder / filename) != sha256:
        raise ValueError('Expected the verified %s %s UNDERTALE 1.08 %s' %
                         ('Windows' if platform == 'windows' else 'macOS', 'Steam' if store == 'steam' else 'GOG', filename))
    paths = sorted(folder.glob('*.ogg'), key=lambda p: p.name.casefold())
    if len(paths) != MUSIC_COUNT or len({p.name.casefold() for p in paths}) != MUSIC_COUNT:
        raise ValueError('Expected exactly %d music files' % MUSIC_COUNT)
    files = [dict(name=path.name, size=path.stat().st_size, sha256=digest(path)) for path in paths]
    return dict(schema=2, notice='names only: file names, sizes in bytes and SHA-256 digests; no game content',
                game='undertale', releases={store: {platform: {version: dict(store=store, depot=platform,
                    data_sha256=sha256, count=len(files), files=files)}}})


def merge(existing, generated):
    """Update only the selected depot; preserve every other record."""
    if existing.get('schema') != 2 or existing.get('game') != 'undertale':
        raise ValueError('Expected a schema 2 UNDERTALE music manifest')
    for store, platforms in generated['releases'].items():
        for platform, versions in platforms.items():
            existing['releases'].setdefault(store, {}).setdefault(platform, {}).update(versions)
    return existing


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).with_name('music-versions.json'))
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--store', choices=('steam', 'gog'), default='steam')
    parser.add_argument('--platform', choices=('windows', 'macos'), default='windows',
                        help='Game depot platform, independent of the host OS')
    args = parser.parse_args()
    generated = generate(args.folder, store=args.store, platform=args.platform)
    existing = json.loads(args.output.read_text(encoding='utf-8')) if args.output.exists() else generated
    text = json.dumps(merge(existing, generated), indent=2) + '\n'
    if args.check:
        if args.output.read_text(encoding='utf-8') != text:
            raise SystemExit('Music manifest differs')
        print('Music manifest matches all %d files.' % MUSIC_COUNT)
    else:
        args.output.write_text(text, encoding='utf-8', newline='\n')
        print('Wrote names, sizes and SHA-256 for %d music files.' % MUSIC_COUNT)


if __name__ == '__main__':
    main()
