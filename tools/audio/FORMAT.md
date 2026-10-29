# Experimental DSP-ADPCM container, version 1

Conversion is opt-in. All derived files stay in private build/output folders.
No runtime or release build selects this format yet. Listening approval is
required before enabling it in any shipping builder or game.

The six listening pairs were approved on 2026-10-26:
3 of 11 correct across two blind rounds with good headphones, and "side by
side they are very similar ... i think they both sound as good as each other."
That clears the format listening gate. Native playback, exact timing and
console loop/start/stop/volume/pan validation remain required. Historical
conversion metadata defaults to unapproved; it does not supersede this review.

Private build preparation is available with
`platform/3ds/build.sh undertale o3ds audio-convert data="<own-game-folder>"` (or DELTARUNE), or
`py -3 -I tools/audio/prepare.py --data <own-game-folder> --out <private-output>`.
The build option uses a separate `-audioconvert` build directory and writes
private assets under `build/audio-adpcm-<game>/`. It does not enable playback
or copy these assets to an SD card. The builder's source mirror includes the
audio tools; the normal wizard and release defaults do not invoke conversion.

The preparation manifest covers every SOND slot (including absent ones) and
every OGG file under the input root. It identifies each data.win and sound
source by SHA-256, stores one object per unique source, and records fallback
reasons for missing/unsupported sources or unequal Vorbis end-granule and
decoded frame counts. Cached objects are reused only when their source,
encoder/version signature and payload hash agree. Each manifest explicitly
sets `approved=false` and `runtime_enabled=false`. Runtime Tremor parity,
source-invalidation lookup and native playback remain separate gates.

`prepare.py --mono-sfx` additionally applies `exact-dual-mono-wave-v2` to
embedded WAV sources. Original PCM8/PCM16 stereo channel bytes must match
exactly. Only then does the converter encode one channel; source rate, frame
count and original channel count stay exact. True stereo and unsupported WAVs
remain PCM fallbacks. This opt-in is bound into the encoder/cache signature.
It is not yet enabled by normal PC-builder delivery or game SFX playback.
The projected object key is SHA-256(policy ASCII, NUL, original source blob),
separate from an unprojected object's source SHA-256, so aliases used for
stereo music cannot select a mono payload accidentally.

All integers are little endian except the DSP frame payload itself, whose
sample nibbles are in hardware order (high nibble first). A DSP frame is
8 B: predictor/scale, then 14 signed 4-bit residuals. Each channel has its
own 16 signed 16-bit predictor coefficients. Reconstructed samples use
`clamp16((residual * 2^scale * 2048 + coef0 * hist0 + coef1 * hist1 + 1024) >> 11)`.
The shift is arithmetic. `hist0` is the last output sample.

The file starts with a 128 B header:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | 4 B | `DTAD` |
| 4 | u32 | version, 1 |
| 8 | u32 | header size, 128 B |
| 12 | u32 | source rate in Hz |
| 16 | u32 | encoded channel count, 1 or 2; source count is retained separately for mono projection |
| 20 | u64 | exact source sample frames, excluding final-frame padding |
| 28 | u32 | block capacity, 2,044 frames (146 DSP frames) |
| 32 | u32 | block count, ceiling(sample frames / block capacity) |
| 36 | u32 | flags, 0 |
| 40 + channel * 40 | s16[16] | that channel's coefficients |
| 72 + channel * 40 | 8 B | reserved start/loop context; zero in version 1 |

Remaining header bytes are zero. Blocks follow in source-time order, and
within each block, channel 0 then channel 1. Each channel record starts
with an 8 B context: u16 predictor/scale (equal to payload byte 0), s16
history0, s16 history1, u16 reserved zero. Its payload follows immediately:
`8 * ceiling(min(block capacity, remaining sample frames) / 14)` B. The
final DSP frame's padding is never included in a playback sample count.

Block histories permit reads without decoding earlier blocks. Arbitrary
sample seeks still require an exact runtime strategy: libctru sets encoding
per physical channel, so a PCM prefix cannot simply precede ADPCM in one
buffer queue. No sample may be lost or added; unsupported seeks must retain
the fallback path. The initial/loop history is zero, preserving the current
runtime's whole-file loop-to-zero semantics. Do not invent loop points from padding.

Stereo records are independent mono ADPCM streams. libctru documents
`NDSP_FORMAT_MONO_ADPCM` and per-channel coefficients, with no stereo ADPCM
format constant. Stereo playback must preserve the original mix and account
for both physical channels; downmixing is not authorized by this format.

Public interface references: [libctru channel formats and coefficients](https://raw.githubusercontent.com/devkitPro/libctru/master/libctru/include/3ds/ndsp/channel.h),
[libctru wave buffers and ADPCM contexts](https://raw.githubusercontent.com/devkitPro/libctru/master/libctru/include/3ds/ndsp/ndsp.h),
and [3dbrew's channel/seek context description](https://3dbrew.org/wiki/BCSTM).

This layout is a prototype contract, not a replacement for input validation.
A runtime reader must reject inconsistent lengths, overflowing arithmetic,
invalid predictors/scales and unsupported versions, and take the existing
Vorbis/PCM path. Source identity/invalidation belongs to the build manifest.

The production C reader is `ctr_adpcm_format.h` / `z_adpcm_format.c`.
It validates the header and exact file length, reads independent blocks
through an owner-supplied bounded read callback, and exposes channel payloads
and histories for native NDSP queueing. Its bounded PCM span decoder handles
sample-exact reads inside a block. That helper alone does not solve mixing
PCM and ADPCM in one NDSP queue. Only parsed `CtrAdpcmInfo` values and blocks
returned by this reader belong at its API boundary. Later block histories
are container inputs; native playback cannot prove their continuity without
CPU decoding, so source identity and build-time validation remain necessary.

The opt-in UNDERTALE `adpcmprobe` build diagnostic checks native DSP decoding
before game audio starts. It plays each selected channel/block beside its
PCM reference and captures the final mix for a byte comparison. It is not
the game's ADPCM streaming/cache backend. The ordinary build defaults do
not run it; private probe assets/captures live under the Azahar game folder.

The native queue primitive is `ctr_adpcm_player.h` / `z_adpcm_player.c`.
It uses four blocks: 4,704 B linear for mono or 9,408 B for stereo SD streams,
versus the existing 65,536 B per PCM stream. A compressed resident SFX instead
queues pointers into its pinned linear DTAD, using one block of ordinary
scratch for validation. These are component budgets, not measured game savings:
the game adapter has not yet replaced the old allocations.

The owner leases one physical channel per source channel. It prepares an SD
request under its lock, reads and validates outside that lock, then commits
under the lock. The DSP callback publishes ready buffers and paired pause
changes in one update. It performs no SD reads or software decoding. A stale
request cannot commit after stop/restart; its source/storage remain pinned
until the read returns. Final buffers exclude padding and a loop queues the
original block-zero history, without inserting samples. Native entry currently
accepts only exact block boundaries; the game adapter must retain exact legacy
playback for other seeks. Channel admission, stop tails and seam correction
belong to that adapter. Their gameplay integration is still pending.

Host device tests cover mono/stereo completion, 50 loops, frame positions,
pause, explicit mix/rate changes, zero-copy resident playback, short reads,
refill stalls and stale read cancellation. The updated native diagnostic
compares up to six consecutive blocks against PCM through the production
queue, refilling its four-block ring outside the callback. Its DSP-on result
and linked layout/cost gates remain pending.

The first six-block queue batch passed on DSP-on Azahar Old64: 36 cases,
273,927 nonzero sample pairs, 0 unequal samples. At least 20 cases crossed
the four-block ring and required a refill. The session retained 0 B, and the
ordinary Snowdin benchmark completed afterward. Both UT model builds had
0 warnings and 0 full layout failures. This validates the native queue;
it does not establish the game's voice, seek, stop or loop behavior.

`stage.py` turns a prepared cache into a separate private SD asset folder:
`objects/<source SHA-256>.dtad`, `index/<data.win SHA-256>.dtai`, and a staging
receipt. `build.sh audio-convert` now prepares that folder as well as the
converter cache. Default builds still do not select this runtime path.
The encoder and original-source identities and prepared payload hashes must
match before staging. Unsupported/ineligible slots stay explicit fallbacks.
No game files are changed. Existing unowned output folders are rejected.

The DTAI v1 index is little-endian, with a 128 B header:

| Offset | Field |
| --- | --- |
| 0 | `DTAI` magic, then u32 version 1 |
| 8 | u32 header size 128 B, u32 record size 128 B |
| 16 | u32 SOND slot count, u32 external file slot count |
| 24 | u32 body size, u32 string-table size |
| 32 | 32 B data.win SHA-256 |
| 64 | 20 B body SHA-1; remaining header bytes zero |

The body is SOND records in original slot order, then external-file records,
then NUL-terminated UTF-8 paths. An unavailable record is entirely zero.
A valid record contains u32 flags (valid=1, embedded=2, identical ADPCM
channels=4, exact mono projection=8), frames, rate, encoded channels, DTAD bytes and string offset at offsets
0â€“20; u64 source offset at 24; u32 source bytes and original channels at
32â€“36; 32 B source/policy key at 40; 20 B payload SHA-1 at 72 and original
source SHA-1 at 92. Bytes 112â€“127 are zero in legacy records. Projected mono
records instead contain u64 containing-source-file bytes at 112 and u32
source bit depth (8 or 16) at 120, and u32 (largest adjacent rendered PCM16
step + 1) at 124. Values 1â€“65,536 encode steps 0â€“65,535; zero rejects the
obsolete projection without prepared seam statistics. Such records must
be embedded, store one channel and retain original source channels=2. Paths are relative to the game
root and identify the original OGG/WAV or containing data.win/audiogroup file.
The source hash covers exactly sourceBytes at sourceOffset, so a changed
audiogroup is detectable even with an unchanged data.win. File records have
zero sourceOffset. Preparation and post-copy receipt publication independently
verify the exact projection, frame/rate metadata and policy key against the
original source span. They also independently render and verify the seam
statistic, eliminating a whole-sound seam scan from first play. Neither projection flag authorizes downmixing
other stereo sources or changing their panning policy.

`CtrAdpcmCatalog` validates identity, counts, lengths, body hash, records and
paths before exposing lookups. It borrows the loaded index and allocates
nothing. Body integrity alone does not validate the corresponding SD source
or payload: the game adapter must verify their identities before selecting
native playback.

`CtrAdpcmStore` supplies the SD-file side of that selection. It tries the
data.win directory and parent for the index, retaining verified metadata
in caller-supplied ordinary heap. Run `receipts.py --sd-root <game-folder>
--assets <game-folder>/audio-adpcm --index <installed-index>` **after the final
copy to the card**. Preparation verifies source and payload SHA-1 and sizes
against the catalog, checks the payload header, captures actual destination
metadata, and atomically publishes `receipts/<data.win SHA-256>.dtar`.
Cancellation, read failure or a changed input leaves no published receipt.
Re-run preparation after replacing any source, payload or catalog. Receipts
made for a staging folder do not authorize a later copy with different times.

DTAR v1 has a 128 B little-endian header: magic `DTAR` at 0; u32 version,
header bytes, record bytes, record count, total bytes and timestamp scheme
(1) at 4â€“24; zero u32 at 28; full catalog SHA-1 at 32; body SHA-1 at 52;
u64 receipt timestamp probe in UTC and FAT-local calendar form at 72/80;
data.win SHA-256 at 88; 8 zero bytes at 120. Its 32 B records correspond to
the exact catalog slots, binding paths, sizes and content identities through
the full catalog hash. Each external or projected-mono embedded record contains
four u64 timestamps: source UTC/FAT, then payload UTC/FAT. Projected source
timestamps cover the containing data.win/audiogroup file; its exact size is
bound in the catalog. Legacy embedded/unavailable records are zero.
The receipt's timestamp is a known, exactly FAT-representable probe
(2001-02-03 04:05:06 UTC). Preparation packs and round-trips the actual
destination timestamps as FAT fields with 2 s precision. Runtime reads the
probe with libctru `archive_getmtime()` to select the checked FAT-local or
emulator UTC representation; an unknown form rejects the receipt.

The adapter loads the catalog and receipt once during initialization. The
opener checks source size and timestamp, payload size and timestamp, and
only its bounded 128 B DTAD header for exact rate/channels/source frames.
It performs no whole-track hash. Missing/stale/partial receipts immediately
fall back and log status 12; changed source metadata is 13, payload metadata
14. Metadata-preserving edits (including FAT's 2 s ambiguity) require explicit
re-preparation; this cache is not an adversarial integrity boundary. The
game data remains read-only. The file uses a dedicated descriptor with bounded
random access. No full-track PCM or compressed allocation is made. The
opener owns metadata checks; the audio worker owns block reads outside the
player lock. The descriptor must outlive every outstanding request, even
after native stop, and can outlive the catalog once its borrowed asset has
been consumed. Failure reasons distinguish an absent index, invalid index,
ordinary-heap exhaustion, unconverted source and stale source or payload.
`CtrAdpcmStore_openSound` permits only explicit projected mono records, by
original SOND ID. It checks containing-source size/timestamp and the payload's
bounded header without opening/scanning the original WAV. This component has
synthetic admission/fallback tests. The combined Option B adapter now calls it
before original play and synchronous UT preload. The queue route stays nonblocking:
projected entries not preloaded defer to first play, instead of sending WAVs to the
PCM worker. ARM/gameplay and first-play latency gates are pending.
Catalogs with projected records retain their original SOND slots and receipt
slots in ordinary heap. Music-only catalogs keep the existing compaction;
retained metadata must be included in later net-memory measurements.
Other embedded sources retain the existing fallback. Game stream selection,
channel leases, loop-seam handling and normal builder delivery are pending.

For the approved Azahar measurement only, `build.sh adpcmmusic
adpcmtestmtime` enables `CTRPORT_ADPCM_TEST_UNKNOWN_MTIME`. It is OFF by
default, absent from normal release compile definitions, and gets a separate
build directory. The test path requires the existing emulator query
(`svcGetSystemInfo(0x20000, 0)`) to succeed and identify an emulator before
treating the service's raw zero timestamp (libctru converts it to 946684800 s)
as unknown. Receipt/catalog identities, size and bounded exact audio metadata
remain required. It logs `ADPCM TEST ONLY` and `MTIME CHECK NOT TESTED`.
Azahar cannot validate stale-mtime rejection; hardware rows use the release
path with the switch OFF and real FAT timestamps. The switch-off proof
compares all mapped ELF bytes/extents against the no-switch frozen build,
exempting only the fixed 40 B build ID. No emulator executable is modified.

## Combined Option B candidate (charter rev 2, 2026-10-28)

Music decodes true-stereo ADPCM through the existing core-1 PCM feeder. Only
verified dual-mono embedded WAV SFX use resident mono DTAD; true-stereo SFX
remain PCM. SFX decode small PCM pieces through the existing feeder, including
its stop tails and seam corrections. Native SFX is a later round. Source counts,
lengths, positions and logical voice behavior remain exactness gates.

`CtrAdpcmPcm` is the incremental resident decoder for those pieces. Opening
it checks the header and every block descriptor/predictor without copying
the payload. The cursor borrows immutable pinned bytes, allocates nothing
and performs no I/O. Consecutive reads retain histories; a seek restarts at
the nearest stored block, then discards the exact prefix. The caller owns
the output storage, serialization, lifetime and tick/core measurements.
`decodedFrames` reports both delivered frames and discarded seek work, so
successive small pieces cannot hide repeated prefix decoding in a CPU report.
No conversion source or approved payload changes for this path.

The combined adapter installs compressed payloads in the original linear cache,
so original eviction remains the sole payload owner. Metadata/cursors use ordinary
heap. A 98,304 B mono piece pool is allocated only on first successful admission;
pitch growth can allocate additional leased ring generations, counted separately.
No allocation or I/O occurs in the DSP callback. The shared production-piece
tests cover leases, delayed callbacks, pitch 8, seeks, loops and stop tails.
The original 196,608 B stereo-piece prototype remains separate and unselected.
Measured Old-model net memory, CPU/latency and console gates remain pending.
