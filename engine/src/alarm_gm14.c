// UNDERTALE's alarm setter (colflip, alarm14; alarm_gm14.h): GameMaker: Studio 1.4's rounding of the value that
// `alarm[i] = v` stores, or RValue_toInt32's truncation for DELTARUNE.
//
// Late code only (.text.late.alarm14, listed last in platform/3ds/late_reserve.ld), so every function linked before it
// keeps its bytes and address. The RValue conversions are this file's own copies (rvalue.h's static ones, renamed so
// the map tells them apart from vm_builtins.c's), linked in .late.text too (*alarm_gm14.o(.text .text.*)).
#define RValue_toRealOther Alarm14_toRealOther
#define RValue_toInt32Other Alarm14_toInt32Other
#include "rvalue.h"
#define ALARM_GM14_DEFINE
#include "alarm_gm14.h"
