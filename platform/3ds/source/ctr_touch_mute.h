#ifndef CTR_TOUCH_MUTE_H
#define CTR_TOUCH_MUTE_H
#include <stdint.h>
struct Runner;
// Sample owner clears first, including reset/physical takeover. A mark authorizes one logical play only.
void CtrTouchMute_clear(void);
void CtrTouchMute_mark(struct Runner* runner, uint32_t instance, int32_t object,
                       int32_t key, const char* script);
#endif
