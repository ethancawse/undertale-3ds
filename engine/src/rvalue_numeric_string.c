// Keep the null guard outside the existing conversion bodies. A normal call
// replaces strtod/strtof with the same ABI; the 3DS section follows warm text.
#include <stdlib.h>
#include "common.h"
#include "real_type.h"

#if defined(__GNUC__) && defined(__3DS__)
__attribute__((noinline, section(".heapfrag")))
#endif
GMLReal RValue_numericStrtod(const char* string, char** end) {
    if (string == NULL) {
        if (end != NULL) *end = NULL;
        return 0.0;
    }
    return GMLReal_strtod(string, end);
}
