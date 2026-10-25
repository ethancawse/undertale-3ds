#ifndef CTR_SLOT_DIAG_H
#define CTR_SLOT_DIAG_H

#include "data_win.h"
#include "renderer.h"

// Diagnostic builds only: no counters, calls or renderer fields in release.
enum CtrSlotKind { CTR_SLOT_TPAG, CTR_SLOT_SPRITE };
enum CtrSlotEvent { CTR_SLOT_APPEND_ATTEMPT, CTR_SLOT_APPENDED, CTR_SLOT_REUSE,
                    CTR_SLOT_RELEASE, CTR_SLOT_GROW_ATTEMPT, CTR_SLOT_GROWN };

#ifdef CTRPORT_SLOT_DIAGNOSTICS
void CtrSlotDiag_begin(Renderer* renderer, size_t surfaceBytes);
void CtrSlotDiag_room(void);
void CtrSlotDiag_event(DataWin* dw, enum CtrSlotKind kind, enum CtrSlotEvent event);
void CtrSlotDiag_end(DataWin* dw);
#else
#define CtrSlotDiag_begin(...) ((void) 0)
#define CtrSlotDiag_room(...) ((void) 0)
#define CtrSlotDiag_event(...) ((void) 0)
#define CtrSlotDiag_end(...) ((void) 0)
#endif

#endif
