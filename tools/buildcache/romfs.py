"""Build host-independent raw/IVFC RomFS images for 3dsxtool and makerom.

Every directory's entries use ascending UTF-16 code-unit order, case-sensitive,
with no locale or Unicode normalization. Ignore dot entries on every host;
reject symlinks/special files. Timestamps and host attributes are not serialized.
Format references: devkitPro/3dstools src/romfs.cpp and
3DSGuy/Project_CTR makerom/src/romfs_gen.c (RomFS and IVFC wire formats).
This module uses only the standard library, including on Python 3.8.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import stat
import struct
import tempfile

NONE = 0xffffffff
BLOCK = 4096


def align(value, boundary):
    return (value + boundary - 1) & -boundary


def buckets(count):
    if count < 3:
        return 3
    if count < 19:
        return count | 1
    while any(count % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        count += 1
    return count


def path_hash(parent, name):
    value = parent ^ 123456789
    for unit, in struct.iter_unpack('<H', name):
        value = ((value >> 5) | (value << 27)) & NONE
        value ^= unit
    return value


def records(source):
    dirs, files = [], []

    def visit(path, name, parent):
        node = dict(path=path, name=name.encode('utf-16le'), parent=parent, dirs=[], files=[])
        dirs.append(node)
        for entry in sorted(path.iterdir(), key=lambda p: p.name.encode('utf-16be')):
            if entry.name.startswith('.'):
                continue
            mode = entry.lstat().st_mode
            if stat.S_ISLNK(mode):
                raise ValueError('RomFS input contains a symlink: ' + str(entry))
            if stat.S_ISDIR(mode):
                node['dirs'].append(visit(entry, entry.name, node))
            elif stat.S_ISREG(mode):
                item = dict(path=entry, name=entry.name.encode('utf-16le'), parent=node,
                            size=entry.stat().st_size)
                node['files'].append(item)
                files.append(item)
            else:
                raise ValueError('RomFS input is not a regular file: ' + str(entry))
        return node

    if source.is_symlink() or not source.is_dir():
        raise ValueError('RomFS input must be a directory without symlinks')
    root = visit(source, '', None)
    root['parent'] = root
    for nodes, header in ((dirs, 24), (files, 32)):
        offset = 0
        for node in nodes:
            node.update(offset=offset, sibling=NONE)
            offset += header + align(len(node['name']), 4)
        if offset > NONE:
            raise ValueError('RomFS metadata exceeds 32-bit offsets')
    for node in dirs:
        for children in (node['dirs'], node['files']):
            for a, b in zip(children, children[1:]):
                a['sibling'] = b['offset']
    return dirs, files


def table(nodes, directory):
    heads = [NONE] * buckets(len(nodes))
    metadata = bytearray()
    for node in nodes:
        parent = node['parent']['offset']
        bucket = path_hash(parent, node['name']) % len(heads)
        if directory:
            children = [node[k][0]['offset'] if node[k] else NONE for k in ('dirs', 'files')]
            metadata.extend(struct.pack('<6I', parent, node['sibling'], *children,
                                        heads[bucket], len(node['name'])))
        else:
            metadata.extend(struct.pack('<IIQQII', parent, node['sibling'], node['data'],
                                        node['size'], heads[bucket], len(node['name'])))
        metadata.extend(node['name'])
        metadata.extend(bytes(align(len(metadata), 4) - len(metadata)))
        heads[bucket] = node['offset']
    return struct.pack('<%dI' % len(heads), *heads), metadata


def write_raw(source, stream):
    dirs, files = records(source)
    size = 0
    for node in files:
        node['data'] = align(size, 16)
        size = node['data'] + node['size']
    sections = (*table(dirs, True), *table(files, False))
    header = [40]
    offset = 40
    for section in sections:
        header.extend((offset, len(section)))
        offset += len(section)
    data_start = align(offset, 16)
    header.append(data_start)
    stream.write(struct.pack('<10I', *header))
    for section in sections:
        stream.write(section)
    stream.write(bytes(data_start - offset))
    for node in files:
        start = data_start + node['data']
        stream.write(bytes(start - stream.tell()))
        with node['path'].open('rb') as reader:
            shutil.copyfileobj(reader, stream, 1024 * 1024)
        if stream.tell() != start + node['size']:
            raise ValueError('RomFS input changed while packaging: ' + str(node['path']))


def hashes(stream, size):
    stream.seek(0)
    result = bytearray()
    for offset in range(0, size, BLOCK):
        data = stream.read(min(BLOCK, size - offset))
        if len(data) != min(BLOCK, size - offset):
            raise ValueError('Truncated RomFS input')
        result.extend(hashlib.sha256(data.ljust(BLOCK, b'\0')).digest())
    return bytes(result)


def write_ivfc(raw, stream):
    import io
    raw.seek(0, os.SEEK_END)
    raw_size = raw.tell()
    bottom = hashes(raw, raw_size)
    middle = hashes(io.BytesIO(bottom), len(bottom))
    master = hashes(io.BytesIO(middle), len(middle))
    header = bytearray(struct.pack('<4sII', b'IVFC', 0x10000, len(master)))
    logical = 0
    for size in (len(middle), len(bottom), raw_size):
        header.extend(struct.pack('<QQII', logical, size, 12, 0))
        logical = align(logical + size, BLOCK)
    header.extend(struct.pack('<II', 92, 0))
    header.extend(bytes(align(len(header), 16) - len(header)))
    header.extend(master)
    # Header/master, level 3 data, level 1 hashes, level 2 hashes.
    stream.write(header)
    stream.write(bytes(align(stream.tell(), BLOCK) - stream.tell()))
    raw.seek(0)
    shutil.copyfileobj(raw, stream, 1024 * 1024)
    stream.write(bytes(align(stream.tell(), BLOCK) - stream.tell()))
    for data in (middle, bottom):
        stream.write(data)
        stream.write(bytes(align(stream.tell(), BLOCK) - stream.tell()))


def build(source, raw_path, ivfc_path):
    source = Path(source)
    if source.is_symlink():
        raise ValueError('RomFS input contains a symlink: ' + str(source))
    source = source.resolve()
    outputs = [Path(raw_path).absolute(), Path(ivfc_path).absolute()]
    if outputs[0].resolve() == outputs[1].resolve():
        raise ValueError('RomFS output paths must differ')
    for path in outputs:
        if source == path.resolve() or source in path.resolve().parents:
            raise ValueError('RomFS output must be outside the input directory')
        path.parent.mkdir(parents=True, exist_ok=True)
    temporary = []
    try:
        for path in outputs:
            fd, name = tempfile.mkstemp(prefix=path.name + '.', suffix='.part', dir=path.parent)
            os.close(fd)
            temporary.append(Path(name))
        with temporary[0].open('w+b') as raw:
            write_raw(source, raw)
            raw.flush()
            with temporary[1].open('wb') as ivfc:
                write_ivfc(raw, ivfc)
        for src, dst in zip(temporary, outputs):
            os.replace(str(src), str(dst))
    finally:
        for path in temporary:
            if path.exists():
                path.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True)
    parser.add_argument('--raw', required=True)
    parser.add_argument('--ivfc', required=True)
    args = parser.parse_args()
    build(args.source, args.raw, args.ivfc)


if __name__ == '__main__':
    main()
