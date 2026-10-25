#ifndef CTR_ARRAY_H
#define CTR_ARRAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "stb_ds.h"

// Checked stb growth: identical successful capacities, unchanged pointer and
// header on failure. The append returns a slot only after reserving it.
bool CtrArray_reserve(void* address, size_t elem, size_t need);
void* CtrArray_add(void* address, size_t elem, size_t count);
void CtrArray_failed(void);
bool CtrArray_resize(void* address, size_t elem, size_t count);
bool CtrArray_putU32(uint32_t** address, uint32_t value);
int32_t CtrArray_payload(uint8_t** address, const void* bytes, size_t count);
void CtrArray_clear(void* array, bool* dropped);
#ifdef CTRPORT_DEBUG_RECORD_FAIL_AFTER
void CtrArray_failNext(void);
#endif

#define CTR_ARRAY_RESERVE(a, n) CtrArray_reserve(&(a), sizeof(*(a)), (size_t) (n))
#define CTR_ARRAY_ADD(a, n) CtrArray_add(&(a), sizeof(*(a)), (size_t) (n))
#define CTR_ARRAY_PUT(a, value) \
    __extension__ ({ __typeof__(a) ctr_slot_ = CTR_ARRAY_ADD(a, 1); \
                    if (ctr_slot_ != NULL) { *ctr_slot_ = (value); } ctr_slot_ != NULL; })

#endif
