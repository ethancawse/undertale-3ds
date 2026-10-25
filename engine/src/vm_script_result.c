// The result of a script call, made safe to keep once the callee's frame is freed (runScriptFrameWith in vm.c): what
// RValue_stealOwnershipOrCopy gives, but for a string the result doesn't own that is a static string
// (RValue_isStaticString: the data.win's string table or the one-character strings). Such a string outlives every
// value, so the caller keeps it as the view it is, as storeStackValueIntoSlot and popScriptArgs do. DELTARUNE's
// stringset (return argument0), which every string stringsetloc gives goes through, returns one: copying it cost a
// strdup and a free a call.
//
// runScriptFrameWith calls it only for a string, array, method or struct result that owns nothing; any other result
// needs nothing and stays on the inline path. (The first version called it for every result: on an Old 2DS with
// the ports on, Queen's step and the titan climb's draw were 0.1-0.35 ms a frame slower, at a few hundred script calls
// a frame. Hardware pays for a call this far from the hot code; Azahar doesn't show it.) It takes the result by
// address: a call with the value in and out was too big for the inlined copies' old size.
//
// Late code and constants (platform/3ds/late_reserve.ld): vm.c's warm code keeps its place and size. The copies are
// RValue_makeIndependent's, spelled out so that nothing lands outside the late sections.

#include <stdlib.h>
#include <string.h>

#include "rvalue.h"
#include "utils.h"

#ifdef __3DS__
#define LATE_CODE __attribute__((noinline, section(".text.late.noports3")))
#define LATE_CONST __attribute__((section(".rodata.late.noports3")))
#else
#define LATE_CODE
#define LATE_CONST
#endif

LATE_CONST static const char strdupFailed[] = "FATAL: strdup() failed for a script result (%u bytes)\n";

LATE_CODE void VM_scriptResultInPlace(RValue* result) {
    if (result->ownsReference) return;
    if (result->type == RVALUE_STRING && result->string != nullptr) {
        if (RValue_isStaticString(result->string)) return;
        char* copy = strdup(result->string);
        if (copy == nullptr) {
            logError(strdupFailed, (unsigned) strlen(result->string));
            abort();
        }
        *result = RValue_makeOwnedString(copy);
        return;
    }
    if (result->type == RVALUE_ARRAY && result->array != nullptr) {
        GMLArray_incRef(result->array);
        result->ownsReference = true;
#if IS_WAD17_OR_HIGHER_ENABLED
    } else if (result->type == RVALUE_METHOD && result->method != nullptr) {
        GMLMethod_incRef(result->method);
        result->ownsReference = true;
#endif
    } else if (result->type == RVALUE_STRUCT && result->structInst != nullptr) {
        Instance_structIncRef(result->structInst);
        result->ownsReference = true;
    }
}
