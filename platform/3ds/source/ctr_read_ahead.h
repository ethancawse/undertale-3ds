#ifndef CTRPORT_CTR_READ_AHEAD_H
#define CTRPORT_CTR_READ_AHEAD_H

// Background read-ahead for the music streams' files.
//
// A save on a slow SD card can hold the card for seconds, and every read in
// the meantime waits for it. A stream that reads its file as it decodes then
// runs dry and the music stutters. Here a thread reads each attached file
// ahead of the decoder into a ring of RAM, so the decoder only touches the
// card when it seeks outside what has been read.
//
// The rings are allocated once, in linear memory (the normal heap is the
// tight one), and a file only gets one while a slot is free. A decoder that
// gets none reads its file directly as before.

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct CtrReadAhead CtrReadAhead;

typedef struct {
    uint32_t bytesRead; // by the read-ahead thread
    u64 longestReadTicks; // its slowest single read
    uint32_t waits; // reads that had to wait for the card
    u64 waitTicks;
    u64 longestWaitTicks;
    uint32_t lowestAheadBytes; // least read ahead of a decoder once it was filled (UINT32_MAX when none)
} CtrReadAheadStats;

// Starts the read-ahead thread and allocates the rings. False (and no
// read-ahead) when there's not enough memory or no thread.
bool CtrReadAhead_init(int core, s32 priority);
void CtrReadAhead_shutdown(void);
uint32_t CtrReadAhead_slotBytes(void);
int CtrReadAhead_slotCount(void);

// Takes over reading file from offset pos. On success the read-ahead owns
// file and fileBuffer (its setvbuf buffer, may be NULL) and closes them in
// CtrReadAhead_detach; NULL when no ring is free (file is left alone).
CtrReadAhead* CtrReadAhead_attach(FILE* file, void* fileBuffer, long pos);
// fread-like: returns fewer than n bytes only at the end of the file or on
// a read error.
size_t CtrReadAhead_read(CtrReadAhead* ra, void* dst, size_t n);
bool CtrReadAhead_seek(CtrReadAhead* ra, long offset, int whence);
long CtrReadAhead_tell(CtrReadAhead* ra);
// Frees the ring. Doesn't wait for a read in progress: the read-ahead thread
// closes the file once that read is done.
void CtrReadAhead_detach(CtrReadAhead* ra);

// Statistics since the last call.
void CtrReadAhead_takeStats(CtrReadAheadStats* out);

// utpolish (late code, z_utpolish_audio.c): CtrReadAhead_attach, with the descriptor of a file no ring took put
// back where the FILE left it.
CtrReadAhead* CtrReadAhead_attachLate(FILE* file, void* fileBuffer, long pos);

#endif
