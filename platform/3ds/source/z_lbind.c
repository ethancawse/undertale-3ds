// lbind (aotprof, ruling 2026-10-28): aotBind's name test, late code, both games.
//
// The first time a code entry runs, aotBind (engine/src/aot/vm_aot_bridge.inc) walks the whole table of generated
// functions for one generated from it: a strcmp of names for every entry, then aotMatches. On UNDERTALE Old that was
// 376 strcmps (about 24-40k instructions) for each entry with no generated function, and up to as many for one with
// it, at its first run: Photoshop Flowey's 6glove spawn frame (21/1) made five such walks, about 100k instructions.
// aotMatches's own first test is the entry's offset and blob length against the code's; testing those first here
// skips the strcmp for every entry that could not match, so the same entry binds (or none) as before.
//
// Its own unit, so vm.c's code (and DELTARUNE's inlining there) stays as main's but for the call: aotDispatch is 4 B
// shorter, padded back to main's extent in vm_aot_bridge.inc (tools/analysis/check_lbind_slot.py).
#include <string.h>

#include "aot/vm_aot.h"
#include "data_win.h"

bool aotNameDiffers(const AotEntry* entry, const CodeEntry* code) __attribute__((noinline, section(".text.lbind_late")));
bool aotNameDiffers(const AotEntry* entry, const CodeEntry* code) {
    return entry->offset != code->offset || entry->blobLength != code->length || strcmp(entry->codeName, code->name) != 0;
}
