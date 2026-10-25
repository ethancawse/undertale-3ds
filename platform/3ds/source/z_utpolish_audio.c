// A stream's file put back where it was when no read-ahead ring takes it (utpolish).
//
// CtrReadAhead_attach measures the file with lseek(SEEK_END) before it looks for a free ring, and with none free it
// returned NULL with the descriptor still at the end of the file. The decoder then reads the file itself, and its
// FILE's next buffer refill read() at the descriptor's offset: the end. A looping stream took that for its end and
// went back to its start, on a card the other streams were reading, and its queue ran dry. Both rings are often
// taken: UNDERTALE's Waterfall rain is two streams, so Mad Dummy's music started that way on the New 3DS, with an
// underrun.
//
// CtrDecoder_startReadAhead calls this in place of CtrReadAhead_attach. It is a translation unit of its own, with
// its code in utpolish's late section (platform/3ds/late_reserve.ld), so ctr_read_ahead.c's code is unchanged.

#include "ctr_read_ahead.h"

#include <unistd.h>

#define AUDIO_LATE __attribute__((noinline, section(".text.late.utpolish")))

CtrReadAhead* AUDIO_LATE CtrReadAhead_attachLate(FILE* file, void* fileBuffer, long pos) {
    int fd = file != NULL ? fileno(file) : -1;
    off_t before = fd >= 0 ? lseek(fd, 0, SEEK_CUR) : -1;
    CtrReadAhead* ra = CtrReadAhead_attach(file, fileBuffer, pos);
    // The file stays the decoder's: its descriptor goes back to where the FILE left it.
    if (ra == NULL && before >= 0) lseek(fd, before, SEEK_SET);
    return ra;
}
