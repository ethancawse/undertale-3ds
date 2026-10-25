#pragma once

// Expand hooks on existing source lines: allocation error paths encode __LINE__ on ARM.
#include <stdint.h>
#ifdef CTRPORT_TYPED_CENSUS
extern uint32_t aotCensus[];
#define VM_TYPED_CENSUS(n) (aotCensus[n]++)
#else
#define VM_TYPED_CENSUS(n) ((void) 0)
#endif

#if VM_HAS_AOT && !defined(__3DS__)
extern uint32_t aotEpoch;
#define VM_TYPED_INVALIDATE() (aotEpoch++)
#else
// 3DS reset/table invalidation uses the cold linker wrappers in vm_aot_inline.c.
#define VM_TYPED_INVALIDATE() ((void) 0)
#endif
