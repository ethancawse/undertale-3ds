#include "ctr_array.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

#define OOM_LATE __attribute__((noipa, section(".text.late.oomport")))
static const char failureLog[] __attribute__((section(".rodata.late.oomport"))) =
    "Renderer: array growth failed; using allocation fallback\n";
static bool logged __attribute__((section(".bss.late.oomport")));
#ifdef CTRPORT_DEBUG_RECORD_FAIL_AFTER
static bool failNext __attribute__((section(".bss.late.oomport")));
OOM_LATE void CtrArray_failNext(void) { failNext = true; }
#endif

OOM_LATE void CtrArray_failed(void) {
    if (logged) return;
    logged = true;
    logWarn(failureLog);
}

OOM_LATE bool CtrArray_reserve(void* address, size_t elem, size_t need) {
    void* a;
    memcpy(&a, address, sizeof(a));
    size_t cap = (size_t) arrcap(a);
    if (need <= cap) return true;
#ifdef CTRPORT_DEBUG_RECORD_FAIL_AFTER
    if (failNext) {
        failNext = false;
        CtrArray_failed();
        return false;
    }
#endif
    if (cap <= SIZE_MAX / 2 && need < 2 * cap) need = 2 * cap;
    else if (need < 4) need = 4;
    stbds_array_header* h = NULL;
    if (elem != 0 && need <= (SIZE_MAX - sizeof(*h)) / elem) {
        h = realloc(a != NULL ? stbds_header(a) : NULL, sizeof(*h) + elem * need);
    }
    if (h == NULL) {
        CtrArray_failed();
        return false;
    }
    if (a == NULL) {
        h->length = 0;
        h->hash_table = NULL;
        h->temp = 0;
    }
    h->capacity = need;
    a = (char*) h + sizeof(*h);
    memcpy(address, &a, sizeof(a));
    return true;
}

OOM_LATE void* CtrArray_add(void* address, size_t elem, size_t count) {
    void* a;
    memcpy(&a, address, sizeof(a));
    size_t len = (size_t) arrlen(a);
    if (count > SIZE_MAX - len || !CtrArray_reserve(address, elem, len + count)) return NULL;
    memcpy(&a, address, sizeof(a));
    // Empty payloads need no allocation and must not dereference a null header.
    if (a == NULL) return NULL;
    stbds_header(a)->length = len + count;
    return (char*) a + len * elem;
}

OOM_LATE bool CtrArray_resize(void* address, size_t elem, size_t count) {
    if (!CtrArray_reserve(address, elem, count)) return false;
    void* a;
    memcpy(&a, address, sizeof(a));
    if (a != NULL) stbds_header(a)->length = count;
    return true;
}

OOM_LATE bool CtrArray_putU32(uint32_t** address, uint32_t value) {
    uint32_t* slot = CtrArray_add(address, sizeof(value), 1);
    if (slot == NULL) return false;
    *slot = value;
    return true;
}

OOM_LATE int32_t CtrArray_payload(uint8_t** address, const void* bytes, size_t count) {
    size_t at = arrlenu(*address);
    if (count > SIZE_MAX - 3u) return -1;
    size_t padded = (count + 3u) & ~(size_t) 3u;
    if (at > INT32_MAX || padded > (size_t) INT32_MAX - at) return -1;
    if (padded == 0) return (int32_t) at;
    uint8_t* dest = CtrArray_add(address, 1, padded);
    if (dest == NULL) return -1;
    memcpy(dest, bytes, count);
    if (padded > count) memset(dest + count, 0, padded - count);
    return (int32_t) at;
}

OOM_LATE void CtrArray_clear(void* array, bool* dropped) {
    if (array != NULL) stbds_header(array)->length = 0;
    *dropped = false;
}
