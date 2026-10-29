"""Build-time mono projection for exactly identical stereo PCM WAV channels.

Preparation uses this helper when mono_sfx is selected. It preserves
sourceChannels=2 for the original pan policy; true stereo remains ineligible.
No resampling, mixing, threshold test or FFmpeg conversion precedes equality.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dspadpcm as dsp

POLICY = 'exact-dual-mono-wave-v2'


def seam_max_step(rendered: bytes) -> int:
    """The original feeder's adjacent-sample statistic, prepared once on PC."""
    samples = [item[0] for item in struct.iter_unpack('<h', rendered)]
    return max((abs(a - b) for a, b in zip(samples, samples[1:])), default=0)


def object_key(blob: bytes) -> str:
    return hashlib.sha256(POLICY.encode('ascii') + b'\0' + blob).hexdigest()


@dataclass(frozen=True)
class Projection:
    rate_hz: int
    frames: int
    source_bits: int
    pcm16: bytes
    source_sha256: str
    source_pcm_sha256: str


def project(blob: bytes) -> Projection | None:
    """Return one exact channel, or None for a supported non-dual-mono source.

Malformed/unsupported WAVs decline conversion. Match the runtime's first
data chunk and last preceding fmt chunk, but require complete frames and
bounded chunks so a damaged source never authorizes the converted path.
"""
    if blob[:4] != b'RIFF':
        return None
    if len(blob) < 12 or blob[8:12] != b'WAVE':
        raise ValueError('Invalid WAV header')
    end = struct.unpack_from('<I', blob, 4)[0] + 8
    if end != len(blob):
        raise ValueError('WAV RIFF size differs from source')
    at, fmt = 12, None
    while at + 8 <= end:
        tag, length = struct.unpack_from('<4sI', blob, at)
        at += 8
        if length > end - at:
            raise ValueError('WAV chunk exceeds source')
        if tag == b'fmt ':
            if length < 16:
                raise ValueError('Truncated WAV format')
            fmt = struct.unpack_from('<HHIIHH', blob, at)
        elif tag == b'data':
            if fmt is None:
                raise ValueError('WAV data precedes format')
            kind, channels, rate, _, _, bits = fmt
            if kind != 1 or channels not in (1, 2) or bits not in (8, 16) or not 0 < rate <= 192000:
                raise ValueError('Unsupported PCM WAV format')
            width = bits // 8
            if not length or length % (channels * width):
                raise ValueError('WAV data must contain positive whole frames')
            if channels != 2:
                return None
            samples = blob[at:at + length]
            if width == 2:
                if samples[0::4] != samples[2::4] or samples[1::4] != samples[3::4]:
                    return None
                mono = bytearray(length // 2)
                mono[0::2], mono[1::2] = samples[0::4], samples[1::4]
            else:
                if samples[0::2] != samples[1::2]:
                    return None
                # Runtime's WAV reader maps unsigned PCM8 to signed PCM16.
                # ADPCM encoding consumes PCM16; this exact lift adds no loss.
                mono = bytearray(length)
                mono[1::2] = bytes(value ^ 0x80 for value in samples[0::2])
            return Projection(rate, length // (2 * width), bits, bytes(mono),
                hashlib.sha256(blob).hexdigest(), hashlib.sha256(samples).hexdigest())
        at += length + (length & 1)
    raise ValueError('WAV contains no data chunk')


def encode(blob: bytes, out: Path, *, lib=None):
    """Encode an eligible source and validate its complete independent render.

The caller must include POLICY and this module in its conversion identity.
The stored/source channel distinction must survive catalog and receipt
validation before runtime admission; this helper alone does not admit audio.
"""
    projection = project(blob)
    if projection is None:
        raise ValueError('Source is not an exact dual-mono stereo WAV')
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    payload = out.with_suffix('.dtad')
    row = dsp.encode(projection.pcm16, projection.rate_hz, 1, payload, lib)
    rendered, rate, channels = dsp.read(payload)
    if (rate, channels, len(rendered)) != (projection.rate_hz, 1, projection.frames * 2):
        raise ValueError('Mono render changed source rate or frame count')
    row.update(projection=POLICY, source_channels=2, source_bits=projection.source_bits,
        source_sha256=projection.source_sha256, source_pcm_sha256=projection.source_pcm_sha256,
        pcm_sha256=hashlib.sha256(projection.pcm16).hexdigest(),
        rendered_sha256=hashlib.sha256(rendered).hexdigest(),
        seam_max_step=seam_max_step(rendered), timing_eligible=True)
    out.with_suffix('.json').write_text(json.dumps(row, indent=2) + '\n', encoding='utf-8')
    return row
