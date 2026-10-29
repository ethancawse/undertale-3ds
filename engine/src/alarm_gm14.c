// UNDERTALE's alarm setter (colflip, alarm14; alarm_gm14.h): GameMaker: Studio 1.4's rounding of the value that
// `alarm[i] = v` stores, or RValue_toInt32's truncation for DELTARUNE.
//
// Late code only (.text.late.alarm14, listed last in platform/3ds/late_reserve.ld), so every function linked before it
// keeps its bytes and address. The RValue conversions are this file's own copies (rvalue.h's static ones, renamed so
// the map tells them apart from vm_builtins.c's), linked in .late.text too (*alarm_gm14.o(.text .text.*)).
#if defined(ENABLE_WAD16) // freebytes: UNDERTALE calls collision_gm14.c's copies (identical; one per build)
#define RValue_toRealOther Colflip14_toRealOther
#define RValue_toInt32Other Colflip14_toInt32Other
#define RVALUE_OTHER_EXTERN
#else
#define RValue_toRealOther Alarm14_toRealOther
#define RValue_toInt32Other Alarm14_toInt32Other
#endif
#line 9
#include "rvalue.h"
#define ALARM_GM14_DEFINE
#include "alarm_gm14.h"
