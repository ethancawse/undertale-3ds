"""Verify an installed audio catalog once, then atomically publish its receipt.

Run AFTER copying to the SD destination, with ``py -3 -I``. Never writes game
inputs or encoded audio. Receipts are metadata trust records, not per-play
content proofs. The fixed receipt timestamp calibrates archive_getmtime's
FAT-local calendar form versus an emulator's UTC form; unknown forms fail shut.
"""
import argparse
import calendar
import hashlib
import os
from pathlib import Path
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stage
import mono_sfx as mono

HEADER, RECORD = 128, 32
PROBE_UTC = 981173106  # 2001-02-03 04:05:06 UTC, exactly representable by FAT.


def stamps(st):
    utc = int(st.st_mtime)
    fields = time.localtime(utc)
    if not 1980 <= fields.tm_year <= 2107:
        raise ValueError('Timestamp is outside the FAT calendar range')
    # Canonical packed FAT date/time, round-tripped before converting the
    # timezone-free calendar fields to the Unix epoch used by libctru.
    date = (fields.tm_year - 1980) << 9 | fields.tm_mon << 5 | fields.tm_mday
    clock = fields.tm_hour << 11 | fields.tm_min << 5 | fields.tm_sec // 2
    recovered = ((date >> 9) + 1980, (date >> 5) & 15, date & 31,
                 clock >> 11, (clock >> 5) & 63, (clock & 31) * 2)
    if recovered != tuple(fields[:5]) + (fields.tm_sec // 2 * 2,):
        raise ValueError('FAT timestamp does not round-trip')
    return utc, calendar.timegm(recovered)


def check_cancel(cancelled):
    if cancelled():
        raise InterruptedError('Audio receipt preparation cancelled; no receipt published')


def verify(path, expected_bytes, digest, cancelled):
    check_cancel(cancelled)
    before = path.stat()
    if before.st_size != expected_bytes:
        raise ValueError('Installed audio length differs: ' + str(path))
    sha = hashlib.sha1()
    with path.open('rb') as stream:
        while True:
            check_cancel(cancelled)
            block = stream.read(1024 * 1024)
            if not block:
                break
            sha.update(block)
    after = path.stat()
    if ((before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns)
            or sha.digest() != digest):
        raise ValueError('Installed audio changed or hash differs: ' + str(path))
    return after


def verify_span(path, expected_bytes, offset, size, digest, cancelled):
    """Verify a projected embedded source without hashing unrelated container data."""
    check_cancel(cancelled)
    before = path.stat()
    if before.st_size != expected_bytes or offset < 0 or size <= 0 or offset + size > expected_bytes:
        raise ValueError('Installed source container length or span differs: ' + str(path))
    with path.open('rb') as stream:
        stream.seek(offset)
        blob = bytearray()
        while len(blob) < size:
            check_cancel(cancelled)
            part = stream.read(min(1024 * 1024, size - len(blob)))
            if not part:
                raise OSError('Short embedded source read: ' + str(path))
            blob.extend(part)
    after = path.stat()
    if ((before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns)
            or hashlib.sha1(blob).digest() != digest):
        raise ValueError('Installed embedded audio changed or hash differs: ' + str(path))
    return after, bytes(blob)


def publish(sd_root, assets, index_path, *, cancelled=lambda: False):
    root, assets, index_path = (Path(p).resolve() for p in (sd_root, assets, index_path))
    if not index_path.is_relative_to(assets / 'index') or not assets.is_relative_to(root):
        raise ValueError('Receipt must describe this installed SD asset tree')
    index = index_path.read_bytes()
    if len(index) < HEADER or index[:4] != b'DTAI':
        raise ValueError('Invalid installed catalog')
    version, head, rec, sounds, files, body_bytes, strings = struct.unpack_from('<7I', index, 4)
    count = sounds + files
    if (version != 1 or head != HEADER or rec != 128 or sounds > 65535 or files > 65535
            or len(index) > stage.LIMIT or body_bytes < count * 128
            or len(index) != HEADER + body_bytes or strings != body_bytes - count * 128):
        raise ValueError('Invalid installed catalog bounds')
    if any(index[84:128]) or hashlib.sha1(index[128:]).digest() != index[64:84]:
        raise ValueError('Installed catalog checksum differs')
    identity = index[32:64].hex()
    if index_path.name != identity + '.dtai':
        raise ValueError('Catalog filename identity differs')
    directory = stage.below(assets, 'receipts')
    directory.mkdir(exist_ok=True)
    target = stage.below(assets, 'receipts/' + identity + '.dtar')
    temporary = stage.below(assets, 'receipts/' + identity + '.dtar.tmp')
    # A failed/cancelled re-preparation never leaves an older receipt appearing
    # to authorize a partially replaced installation. Game inputs stay read-only.
    target.unlink(missing_ok=True)
    temporary.unlink(missing_ok=True)
    records = bytearray(count * RECORD)
    observed = {}
    checks = []
    try:
        for slot in range(count):
            check_cancel(cancelled)
            row = index[HEADER + slot * 128:HEADER + (slot + 1) * 128]
            flags = struct.unpack_from('<I', row)[0]
            if not flags:
                if any(row):
                    raise ValueError('Nonzero absent catalog row')
                continue
            projected = bool(flags & stage.PROJECTED_MONO)
            if not flags & stage.VALID or flags & ~15 or (not projected and any(row[112:])):
                raise ValueError('Unknown catalog flags')
            if flags & stage.EMBEDDED:
                if slot >= sounds:
                    raise ValueError('Embedded external file')
                if not projected:
                    continue
            if not flags & stage.EMBEDDED and any(row[24:32]):
                raise ValueError('External catalog offset is not zero')
            offset = struct.unpack_from('<I', row, 20)[0]
            if offset >= strings:
                raise ValueError('Catalog path exceeds strings')
            names = index[HEADER + count * 128:]
            end = names.find(b'\0', offset)
            if end < 0:
                raise ValueError('Unterminated catalog path')
            source = stage.below(root, names[offset:end].decode('utf-8'))
            payload = stage.below(assets, 'objects/' + row[40:72].hex() + '.dtad')
            source_bytes = struct.unpack_from('<I', row, 32)[0]
            payload_bytes = struct.unpack_from('<I', row, 16)[0]
            frames, rate, channels = struct.unpack_from('<3I', row, 4)
            source_channels = struct.unpack_from('<I', row, 36)[0]
            if (not frames or not 0 < rate <= 192000 or channels not in (1, 2) or not source_bytes
                    or (source_channels != channels and not projected)
                    or (flags & stage.DUAL_MONO and channels != 2)):
                raise ValueError('Invalid catalog audio metadata')
            source_stat = None
            if projected:
                source_file_bytes, source_bits, seam_max_step = struct.unpack_from('<QII', row, 112)
                seam_max_step -= 1 # zero is an obsolete/unprepared projection contract
                source_offset = struct.unpack_from('<Q', row, 24)[0]
                if (not flags & stage.EMBEDDED or channels != 1 or source_channels != 2 or
                        source_bits not in (8, 16) or not 0 <= seam_max_step <= 65535):
                    raise ValueError('Invalid mono projection contract')
                source_stat, blob = verify_span(source, source_file_bytes, source_offset,
                    source_bytes, row[92:112], cancelled)
                projection = mono.project(blob)
                if (projection is None or (projection.frames, projection.rate_hz, projection.source_bits) !=
                        (frames, rate, source_bits) or mono.object_key(blob) != row[40:72].hex()):
                    raise ValueError('Installed source is not the declared exact mono projection')
                checks.append((source, source_stat))
                observed[source, source_offset, source_bytes, row[92:112]] = source_stat
            blocks = (frames + 2043) // 2044
            last = frames - (blocks - 1) * 2044
            expected = HEADER + (blocks - 1) * channels * 1176 + channels * (8 + ((last + 13) // 14) * 8)
            if payload_bytes != expected:
                raise ValueError('Invalid catalog payload length')
            with payload.open('rb') as stream:
                header = stream.read(HEADER)
            if (len(header) != HEADER or header[:4] != b'DTAD'
                    or struct.unpack_from('<4I', header, 4) != (1, HEADER, rate, channels)
                    or struct.unpack_from('<Q3I', header, 20) !=
                       (frames, 2044, (frames + 2043) // 2044, 0)):
                raise ValueError('Installed payload header differs from catalog')
            values = list(stamps(source_stat)) if projected else []
            inputs = [(payload, payload_bytes, row[72:92])]
            if not projected:
                inputs.insert(0, (source, source_bytes, row[92:112]))
            for path, size, digest in inputs:
                key = (path, size, digest)
                if key not in observed:
                    observed[key] = verify(path, size, digest, cancelled)
                    checks.append((path, observed[key]))
                values.extend(stamps(observed[key]))
            if projected:
                rendered, _, _ = mono.dsp.read(payload)
                if mono.seam_max_step(rendered) != seam_max_step:
                    raise ValueError('Installed mono seam statistic differs')
            struct.pack_into('<4Q', records, slot * RECORD, *values)
        header = bytearray(HEADER)
        struct.pack_into('<4s6I', header, 0, b'DTAR', 1, HEADER, RECORD, count,
                         HEADER + len(records), 1)
        header[32:52] = hashlib.sha1(index).digest()
        header[52:72] = hashlib.sha1(records).digest()
        header[88:120] = index[32:64]
        # Capture actual destination metadata after write, not converter/source
        # timestamps. FAT may round to its 2-second representation.
        temporary.write_bytes(header + records)
        os.utime(temporary, (PROBE_UTC, PROBE_UTC))
        probe = stamps(temporary.stat())
        if probe[0] != PROBE_UTC:
            raise ValueError('Destination timestamp probe was not preserved')
        struct.pack_into('<2Q', header, 72, *probe)
        with temporary.open('r+b') as stream:
            stream.write(header)
            stream.flush()
            os.fsync(stream.fileno())
        os.utime(temporary, (PROBE_UTC, PROBE_UTC))
        if stamps(temporary.stat()) != probe:
            raise ValueError('Destination timestamp form changed')
        for path, old in checks:
            now = path.stat()
            if (now.st_size, now.st_mtime_ns) != (old.st_size, old.st_mtime_ns):
                raise ValueError('Installed file changed before receipt publication')
        check_cancel(cancelled)
        os.replace(temporary, target)  # Publication point: complete record only.
        return dict(path=str(target), bytes=HEADER + len(records), verified_files=len(observed),
                    catalog_sha1=header[32:52].hex(), probe_utc=probe[0], probe_fat=probe[1])
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sd-root', required=True, type=Path)
    parser.add_argument('--assets', required=True, type=Path)
    parser.add_argument('--index', required=True, type=Path)
    args = parser.parse_args()
    result = publish(args.sd_root, args.assets, args.index)
    print('RECEIPT', result['path'], result['bytes'], 'B;', result['verified_files'], 'files verified')


if __name__ == '__main__':
    main()
