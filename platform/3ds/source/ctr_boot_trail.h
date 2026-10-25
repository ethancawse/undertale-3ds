// The boot trail (labboot.txt in the game folder; not boot.txt, which is ctr_chapters.c's): a few lines on the SD
// card saying how far a lab run's start got and how the run ended, written as it goes, so even a run that dies
// without a report (or never opens its log) leaves its last step on the card. See ctr_boot_trail.c.
#ifndef CTRPORT_CTR_BOOT_TRAIL_H
#define CTRPORT_CTR_BOOT_TRAIL_H

// Starts the trail at path ("sdmc:/3ds/undertale-3ds/labboot.txt"); the one from the run before is kept as
// "<path>.prev". Makes the folders it needs.
void CtrBootTrail_open(const char* path);
// Adds "<ms since open> <step>" and flushes it to the card. A no-op before CtrBootTrail_open. Safe from any thread,
// and from an exception handler (no heap, no stdio, no locks).
void CtrBootTrail_mark(const char* step);
// The last step marked, or "not started".
const char* CtrBootTrail_last(void);

#endif
