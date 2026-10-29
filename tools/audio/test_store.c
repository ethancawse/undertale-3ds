#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static unsigned calls;
static int fail;
void* testStore_allocate(size_t bytes) {
    calls++;
    return fail && --fail == 0 ? NULL : malloc(bytes);
}
void testStore_fail(int value) { fail = value; }
unsigned testStore_calls(void) { return calls; }

static int resizeFail, forceMove;
static unsigned resizeCalls;
static size_t resizeBytes;
void testStore_resizeMode(int failAt, int move) {
    resizeFail = failAt; forceMove = move; resizeCalls = 0; resizeBytes = 0;
}
unsigned testStore_resizeCalls(void) { return resizeCalls; }
size_t testStore_resizeBytes(void) { return resizeBytes; }
void* testStore_resize(void* previous, size_t bytes) {
    resizeCalls++; resizeBytes += bytes;
    if (resizeFail && --resizeFail == 0) return NULL;
    if (!forceMove) return realloc(previous, bytes);
    void* next = malloc(bytes);
    if (next) { memcpy(next, previous, bytes); free(previous); }
    return next;
}

static int emulated, overrideTime;
static uint64_t fakeTime;
int testStore_emulator(void) { return emulated; }
int testStore_timestamp(uint64_t* out) {
    if (!overrideTime) return 0;
    *out = fakeTime;
    return 1;
}
void testStore_clock(int emulator, int enabled, uint64_t stamp) {
    emulated = emulator; overrideTime = enabled; fakeTime = stamp;
}
