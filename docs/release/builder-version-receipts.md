# Builder data-version receipts

The builder verifies the source game files, hashes each destination data.win before skipping a copy, and
hashes the final destination before publishing cache/data-version.txt. The reads check the open handle and
path before and after hashing. Size and dates alone never prove equal content. A replacement invalidates
the old receipt before changing data.win; a failed destination verification removes it too.

The existing first two lines identify the table revision, size, stat mtime, digest and exact console path.
The receipt uses the runtime's table revision: Steam/native builds retain revision 2 for their existing
layout; the recognized GOG compatibility build uses revision 3. Both PC delivery and lab staging select
the same conditional table branch from the verified full-file digest. Reading only the table's first
revision define would give Steam a revision-3 receipt that its revision-2 checker rejects. A host C
preprocessor regression test compares the selection for every known input; unknown conditional formats
omit the receipt and leave full hashing available. Runtime code and verification guards do not change.
The PC writes an unbound receipt. On its first use, the console reads the file's real archive modification
time with libctru archive_getmtime (ARCHIVE_ACTION_GET_TIMESTAMP), and adds a third line:

```
FAT1 <archive timestamp>
```

Later uses require the same archive timestamp. A changed timestamp, malformed binding or failed timestamp
query makes the normal checker hash the current file again. A fresh full hash writes an unbound receipt,
which binds on its next use. Legacy two-line caches bind in the same way. The stat mtime check remains;
it is normally zero on the 3DS SD archive and is not the real FAT timestamp.

This is a **builder-verified, timestamp-bound identity**, not independent content verification on every
boot. It is not lossless stale-content detection: replacing data before the first binding, preserving the
archive timestamp, or making same-size edits within its time resolution can retain stale recognition.
FAT timestamps and timestamp conversion have finite resolution. Those cases are explicitly accepted by the
receipt contract; a file size change is still rejected. Archive timestamp errors take the slow path.

The receipt authorizes more than the unverified-data notice. Its digest is also used by the DELTARUNE
chapter guards and launcher hooks, crisp-art packs, room texture plans, UNDERTALE sound preload plans and
the optional Old-model source-tile plan. They retain their size, game-ID, GEN8 and structural checks where
applicable. A stale receipt can select stale generated data. AOT dispatch independently checks the actual
bytecode and variable/function references; the receipt does not bypass those checks.

Copy to SD and Save as files use the same verification and receipt writer. Copying a Save-as-files tree
to a card leaves the portable unbound receipt for the console to bind. Moving it to a different console
path invalidates it. Steam and approved GOG inputs follow the same transfer contract; qualifying another
GOG data version remains a separate task. No GOG recognition row is added by this change.

Lab/dev CSV staging uses the same receipt encoder. It invalidates old receipts before upload, completes
the temporary transfer and rename, freshly hashes the final remote file, checks remote metadata before
and after, then publishes the receipt last. Remote SIZE alone is not content verification. It requires
real MLSD modification timestamps; enabling that capability is a manual deployment step, never automatic.

When a DR receipt is absent or invalid, the cold checker still computes full-file SHA-256. It reads up
to 1 MiB at a time and groups the same 64 SHA rounds in eights. It reports progress after each 64 KiB
hashed. Allocation failure tries 256 KiB, then the original checker and its 64 KiB buffer. File read
errors, short files, close errors and pre/post size/time changes still fail verification. Existing
known-file tables and digest consumers are unchanged. UNDERTALE retains its original implementation.
No record-hash/size shortcut or lazy chapter check is introduced.

Azahar 2126.1.2 cannot qualify the real FAT binding: its archive Control action is stubbed with success
without writing the timestamp output. The probe observed archive_getmtime returning the same 2000-01-01
value before and after a same-size host replacement with a changed host date. A stale receipt therefore
remains trusted there. Host tests exercise changing archive values; actual replacement detection and call
cost require hardware. Emulator call timings describe its stub/IPC behavior, not SD archive performance.
See [the upstream archive control stub](https://github.com/azahar-emu/azahar/blob/2126.1.2/src/core/file_sys/archive_backend.h#L190-L197).
