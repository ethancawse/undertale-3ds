// Elevator-only extent and obsolete-border policy. Existing recorder owns
// operation versioning, drop handling, allocation, replay and screen lifetime.
#ifdef ENABLE_WAD16
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>
#include "ctr_array.h"
#include "ctr_dialogue.h"
#include "instance.h"
#include "renderer.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"
extern const uint32_t Popbot_layout[7];
extern const uint32_t CtrPopupHome_recOffset[1];
extern void CtrPopupHome_rawPush(void*, void*, const void*);
#include "ctr_popup_home_capacity.h"
#include "ctr_popup_home.inc"
#endif
