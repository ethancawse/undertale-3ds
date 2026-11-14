// battletouch's sample hook for other stylus users (change touchui, taps outside battle; 11-08): one pen owner,
// one HID wrapper. z_battletouch.c's Battletouch_take2 calls CtrTouchExt_sample once per input sample while Stylus is
// on (Off returns before it, unchanged) and UNDERTALE's names are found: pen down, lifted or idle, after the buttons'
// keys (a test script's included) and the pen's place are known, before the stylus's keys are merged.
//
// A battle owns the pen: while `battle`, a claim is ignored. Otherwise a claim makes this frame's pulse and hold the
// stylus's keys, merged with the buttons' as the battle's are (a pulse is held this frame and let go at the next
// sample), and the battle's own press lets go (a pen still down does nothing in a battle until lifted). On a `reset`
// frame nothing claimed is merged; the extension lets go of its gesture.
//
// z_battletouch.c defines CtrTouchExt_sample weak, returning false (nothing claims: no change at all); touchui's
// strong definition replaces it.
#ifndef CTR_TOUCH_EXT_H
#define CTR_TOUCH_EXT_H

#include <stdbool.h>
#include <stdint.h>

struct Runner;

typedef struct {
    struct Runner* runner;
    bool pen;               // the pen is down this sample
    bool reset;             // let go of any gesture: input off (Options, a test or playback), suppressHeld (the
                            // buttons' release-to-arm), HOME, sleep or the lid (either end), or since the last
                            // sample a new data file, runner, room or battle controller (the first sample too)
    bool battle;            // a battle controller exists: the battle owns the pen
    int16_t px, py;         // bottom-screen px; lifted, where the pen last was
    uint32_t held, pressed; // the buttons' keys this sample, before the stylus's (ctr_touch_core.h's TOUCH_KEY_* bits)
    void** state;           // the extension's own state: one pointer kept in battletouch's live state (heap), NULL
                            // until the extension sets it; it allocates and owns what it points to
} CtrTouchExtIn;

// true: the extension claims this sample, with *pulse (keys for this frame only) and *hold (keys held this frame),
// both 0 on entry, in TOUCH_KEY_* bits.
bool CtrTouchExt_sample(const CtrTouchExtIn* in, uint32_t* pulse, uint32_t* hold);

// The player's text place, before screens narrows the renderer display for
// a bottom-screen game. Read-only; used to reject hidden Flipped targets.
bool CtrTouchUi_textOther(void);

#endif
