// battletouch's setting for other code (battlesplit shows Original battles on the bottom screen while it is on).
// CtrBattletouch_mode is declared weak: where z_battletouch.c is empty (DELTARUNE) it is absent and
// CtrBattletouch_modeNow() reads Off.
#ifndef CTR_BATTLETOUCH_H
#define CTR_BATTLETOUCH_H

#include <stdint.h>

enum { CTR_BATTLETOUCH_OFF = 0, CTR_BATTLETOUCH_MENUS = 1, CTR_BATTLETOUCH_MENUS_SOUL = 2 };

// The stylus setting in effect this frame: the options menu's, else --ini's, else settings.ini's (none: Off).
__attribute__((weak)) int32_t CtrBattletouch_mode(void);

static inline int32_t CtrBattletouch_modeNow(void) {
    return CtrBattletouch_mode != 0 ? CtrBattletouch_mode() : CTR_BATTLETOUCH_OFF;
}

#endif
