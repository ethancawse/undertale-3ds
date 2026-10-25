#ifndef CTRPORT_DATA_WIN_SLOTS_3DS_H
#define CTRPORT_DATA_WIN_SLOTS_3DS_H

#include "data_win.h"

#if defined(__3DS__) && defined(__GNUC__)
#define TPAGGROW_SLOTS_COLD __attribute__((noinline, section(".text.late.tpaggrow")))
#define TPAGGROW_SLOTS_RODATA __attribute__((section(".rodata.late.tpaggrow")))
#else
#define TPAGGROW_SLOTS_COLD
#define TPAGGROW_SLOTS_RODATA
#endif

// Cold allocation paths; definitions are linked after the engine/draw code.
// Measured Chapter 4 rooms 290-317: 36 live dynamic slots; UT's ten benchmark
// scenes: 0. Keep overflow geometric and diagnostic so new peaks remain safe.
TPAGGROW_SLOTS_COLD uint32_t DataWin_3dsTpagCapacity(uint32_t base);
TPAGGROW_SLOTS_COLD uint32_t DataWin_3dsSpriteCapacity(uint32_t base);
TPAGGROW_SLOTS_COLD void DataWin_3dsGrowSpriteSlots(DataWin* dw, uint32_t firstDynamic);
TPAGGROW_SLOTS_COLD uint32_t DataWin_3dsAllocTpagSlot(DataWin* dw, uint32_t firstDynamic);
TPAGGROW_SLOTS_COLD int32_t DataWin_3dsAppendParsedTpag(DataWin* dw);
TPAGGROW_SLOTS_COLD void DataWin_3dsReserveParsedTpag(DataWin* dw);

#endif
