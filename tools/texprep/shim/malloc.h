// Host stand-in for newlib's <malloc.h> (texprep). ctr_texture_pack.c asks it how much heap is free (mallinfo
// against __ctru_heap_size) and for aligned staging buffers (memalign, freed with free). The host reports an empty
// heap of __ctru_heap_size bytes, so every admission check passes, and staging buffers come from malloc: alignment
// only mattered to the SD card's speed, never to the bytes written.
#ifndef TEXPREP_SHIM_MALLOC_H
#define TEXPREP_SHIM_MALLOC_H

#include <stddef.h>
#include <stdlib.h>

struct texprep_mallinfo {
    size_t arena;
    size_t uordblks;
};
struct texprep_mallinfo texprep_mallinfo(void);
#define mallinfo texprep_mallinfo
#define memalign(alignment, size) ((void) (alignment), malloc(size))

#endif
