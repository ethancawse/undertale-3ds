#ifndef RVALUE_NUMERIC_STRING_H
#define RVALUE_NUMERIC_STRING_H

// Chapter modules retain the libc import spelling and original veneer keys.
// Their linker resolves this import to the safe entry; the main executable's
// libc symbols remain unchanged. real_type.h selects strtod versus strtof.
#if defined(CTR_AOT_MODULE)
#define RValue_numericStrtod GMLReal_strtod
#else
GMLReal RValue_numericStrtod(const char* string, char** end);
#endif

#endif
