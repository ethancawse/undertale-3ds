// Which objects' drawing is dialogue that the "Dialogue box: Bottom screen"
// option moves to the bottom screen, and when it stays on top.
//
// Objects are found by name in the data.win, so one list serves UNDERTALE
// and every DELTARUNE chapter: names a game doesn't have are skipped. Child
// objects follow their parents unless listed as kept on top. Dialogue moves
// only while the dialogue box object (obj_dialoguer) exists and none of the
// blocking objects (battles, speech bubbles, shops, Chapter 3's TV) do, so
// the same writers used for battle text, bubbles and shops stay on top. The
// renderer asks per draw (through a cache of the last instance asked about)
// and moves the draws onto the bottom screen; game logic never sees the
// difference.
#ifndef CTRPORT_CTR_DIALOGUE_H
#define CTRPORT_CTR_DIALOGUE_H

#include <stdbool.h>
#include <stdint.h>

#include "data_win.h"
#include "runner.h"

#define CTR_DIALOGUE_GATE_OBJECTS 4
#define CTR_DIALOGUE_BLOCK_OBJECTS 48

typedef struct {
    uint8_t* redirect;  // per object index: 1 if its Draw events are dialogue
    int32_t objectCount;
    int32_t redirectCount; // objects marked, children included
    int32_t gateObjects[CTR_DIALOGUE_GATE_OBJECTS]; // one must exist: the dialogue box
    int32_t gateCount;
    int32_t blockObjects[CTR_DIALOGUE_BLOCK_OBJECTS]; // none may exist: battles, bubbles, shops
    int32_t blockCount;
} CtrDialoguePolicy;

void CtrDialogue_init(CtrDialoguePolicy* policy, const DataWin* dataWin);
void CtrDialogue_free(CtrDialoguePolicy* policy);

static inline bool CtrDialogue_redirects(const CtrDialoguePolicy* policy, int32_t objectIndex) {
    return 0 <= objectIndex && policy->objectCount > objectIndex && policy->redirect[objectIndex] != 0;
}

// Whether dialogue may move now: the dialogue box exists and no battle,
// speech bubble, shop or other scene that keeps it on top does.
bool CtrDialogue_allowed(const CtrDialoguePolicy* policy, Runner* runner);

// A missing box may be a short cutscene handoff only while the game still
// blocks interaction. Never extend a conversation past the release of control.
bool CtrDialogue_holdAllowed(const CtrDialoguePolicy* policy, Runner* runner);

#endif
