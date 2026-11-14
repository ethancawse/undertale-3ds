// heldslow (change scanout, logaudit G1, 2026-11-08; UNDERTALE only): the held-image record's state, shared by the
// pacer's appended code (ctr_frame_pacer.c, which keeps it) and its printing (z_heldslow.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CTR_HELD_RECORDS 16

// A game-screen image that stayed up longer than its frames' slots planned: the frame that replaced it and the frame
// it was (main.c's liveRunner->frameCount when each was handed to the screen), their rooms, the vblank it was first
// shown on, how many vblanks it stayed up and how many were planned, and the milliseconds since start-up.
typedef struct {
    uint32_t frame, shownFrame;
    int16_t room, shownRoom;
    uint32_t fromVblank;
    uint16_t vblanks, planned;
    uint32_t ms;
} CtrHeldRecord;

// The records kept since the last take (at most CTR_HELD_RECORDS), copied out under the pacer's lock; *more is how
// many more there were. Returns how many were copied.
uint32_t CtrHeldslow_takeHeld(CtrHeldRecord* out, uint32_t* more);

// heldslow G2 (logaudit G2): a frame whose main thread took over 33.4 ms, from its slot's start to the next frame's
// wait: its number and room (at that wait), the milliseconds since start-up, and in tenths of a millisecond (0xFFFF:
// not measured) the whole, then before the step, the step, the draw, audio and the room change, present and the end.
#define CTR_SLOW_RECORDS 16
#define CTR_SLOW_UNKNOWN 0xFFFFu

typedef struct {
    uint32_t frame;
    int16_t room;
    uint16_t tenths[6];
    uint32_t ms;
} CtrSlowRecord;

// As CtrHeldslow_takeHeld, for the slow frames (kept and taken on the main thread).
uint32_t CtrHeldslow_takeSlow(CtrSlowRecord* out, uint32_t* more);

// Since the last take: the bottom transfers into its shown buffer let through outside its blank, dlgblank's
// forced goes; and whether it gave up holding them (for the session).
void CtrHeldslow_takeBottom(uint32_t* outside, uint32_t* forced, bool* gaveUp);
