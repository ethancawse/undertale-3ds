"""Binary-kit-only Windows encoder pin; source exports contain no DLL.

Built with UCRT64 GCC using dspadpcm.CFLAGS, -static-libgcc and
-Wl,--no-insert-timestamp,--image-base,0x180000000 (reproducible PE metadata).
Only KERNEL32 and system UCRT imports are allowed.
The release pack's content table independently checks the resulting audio.
"""
import hashlib
from pathlib import Path

RELATIVE = 'tools/release/builder/bin/dspadpcm.dll'
SIZE = 99443
SHA256 = 'fc91d14240d367436276295e539d9a8b41819085536a7692ccad7a2d858a625d'
SOURCE_SHA256 = 'b084913aa30c3a32c648cff3da76c4e9e3e5a386d8579a49e978a71cc0801c4e'


def validate(path, source_root):
    """Reject a stale source or altered host binary before loading native code."""
    path = Path(path)
    data = path.read_bytes()
    if len(data) != SIZE or hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError('Windows sound encoder does not match its size/SHA-256 pin')
    source = Path(source_root) / 'tools/audio/dspadpcm.c'
    if hashlib.sha256(source.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError('Windows sound encoder does not match the exported C source')
    return path
