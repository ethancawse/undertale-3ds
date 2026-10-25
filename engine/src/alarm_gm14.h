// UNDERTALE's alarm setter (colflip, alarm14): GameMaker: Studio 1.4's runner, bytecode 16 and older, read from
// UNDERTALE.exe (docs/vm-warnings.md, "UNDERTALE's runner (GameMaker 1.4)", "Built-in variable setters").
//
// UNDERTALE.exe's `alarm[i] = v` (the setter at 0x4054a0, registered at 0x40746e) takes the value as a real
// (0x53e380), narrows it to float32 (cvtpd2ps), widens it back and stores it with x87 fistp, which rounds to nearest,
// ties to even (0.8 and 0.9 give 1, 0.5 gives 0, 1.5 and 2.5 give 2, -0.6 gives -1; NaN, infinities and anything out
// of int32 range give the integer indefinite, INT32_MIN, 2147483647 among them, since float32 rounds it to 2^31).
// Every value kind goes that way: int32, int64 and bool as their number, a string as its parsed number (0 for a null
// string), undefined as 0, and an array as its element 0 (here: as 0, what RValue_toReal gives an array; UNDERTALE's
// bytecode never stores one in an alarm). DELTARUNE's runner (DELTARUNE.exe 0x14003eae0) truncates, as
// RValue_toInt32 does, so DELTARUNE keeps that rule.
//
// The code is late code of its own (alarm_gm14.c: .text.late.alarm14, listed last in platform/3ds/late_reserve.ld).
// vm_builtins.c sees only macros from this header: a new declaration there, or a new mention of RValue_toInt32Other,
// changes the order in which GCC emits the file's functions and string constants (and so moves them in the link).
#ifndef _BS_ALARM_GM14_H_
#define _BS_ALARM_GM14_H_

// The alarm value of an RValue (UNDERTALE's rule for GMS 1.4's data, otherwise RValue_toInt32's). On the 3DS an asm
// call of Colflip14_alarmValue: a plain call would add a call graph edge from VMBuiltins_setVariable to new code, which
// reorders GCC's function output (collision.h's Colflip_callScan). The RValue goes in r0-r3 and the value comes back
// in r0, as the AAPCS passes them, and moves to r2, where the inline conversion left it; the call clobbers what the
// ABI lets it; and the nops pad the case to its old span, so that the rest of the function keeps its addresses
// (VMBuiltins_setVariable keeps its size). Other targets call it plainly: their layout is not tracked.
#define COLFLIP_ALARM_PAD "5"
#if defined(__arm__) && !defined(__thumb__) && defined(__3DS__)
#define COLFLIP_ALARM_VALUE(v)                                                                                         \
    __extension__({                                                                                                    \
        union {                                                                                                        \
            RValue r;                                                                                                  \
            uint32_t w[4];                                                                                             \
        } colflipAlarmV_ = {.r = (v)};                                                                                 \
        _Static_assert(sizeof(colflipAlarmV_) == 16, "an RValue is passed in r0-r3");                                  \
        register uint32_t colflipAlarmR0_ __asm__("r0") = colflipAlarmV_.w[0];                                         \
        register uint32_t colflipAlarmR1_ __asm__("r1") = colflipAlarmV_.w[1];                                         \
        register uint32_t colflipAlarmR2_ __asm__("r2") = colflipAlarmV_.w[2];                                         \
        register uint32_t colflipAlarmR3_ __asm__("r3") = colflipAlarmV_.w[3];                                         \
        __asm__("bl Colflip14_alarmValue\n\tmov r2, r0\n\t.rept " COLFLIP_ALARM_PAD "\n\tnop\n\t.endr"                 \
                : "+r"(colflipAlarmR0_), "+r"(colflipAlarmR1_), "+r"(colflipAlarmR2_), "+r"(colflipAlarmR3_)           \
                :                                                                                                      \
                : "ip", "lr", "cc", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7");                                    \
        (int32_t) colflipAlarmR2_;                                                                                     \
    })
#else
int32_t Colflip14_alarmValue(RValue val);
#define COLFLIP_ALARM_VALUE(v) Colflip14_alarmValue(v)
#endif

#ifdef ALARM_GM14_DEFINE
#include <stdbool.h>
#include <stdint.h>

// collision_gm14.h: set by Colflip14_registerAll for bytecode 16 or older (UNDERTALE).
extern bool Colflip_gm14;

// UNDERTALE.exe's conversion of a real to an alarm value: fistp (round to nearest, ties to even) of float32(x), the
// narrowing itself rounding to nearest even (cvtpd2ps under UNDERTALE's MXCSR). Done on the bits with integer
// arithmetic only, so it depends on no floating-point state: not the 3DS's FPSCR, whose rounding mode this engine
// never sets (a 3DS thread may start in round toward zero, which would narrow 3.4999999999 to 3.4999998, not 3.5),
// nor the desktop's MXCSR or x87 control word. (fistp obeys the x87 control word's rounding field, which UNDERTALE.exe
// leaves at nearest: its fldcw sites save and restore it, and the real Undying fight would stall if it truncated.)
static inline __attribute__((always_inline)) int32_t Colflip14_alarmRound(double x) {
    uint64_t b;
    __builtin_memcpy(&b, &x, sizeof(b));
    uint64_t a = b & 0x7FFFFFFFFFFFFFFFull;
    if (a <= 0x3FE0000000000000ull) return 0; // |x| <= 0.5 (0.5 a tie, to even), zeros and subnormals
    if (a >= 0x7FF0000000000000ull) return INT32_MIN; // NaN and infinities: fistp's invalid case, 0x80000000
    // float32 keeps 24 significant bits of the 53: round off the low 29, to nearest even (a carry steps the exponent).
    uint64_t low = a & 0x1FFFFFFFull;
    a -= low;
    if (low > 0x10000000ull || (low == 0x10000000ull && (a & 0x20000000ull))) a += 0x20000000ull;
    // 2^31 or more (2147483647 among them, which float32 rounds to 2^31): fistp's invalid case. -2^31 stores the same.
    if (a >= 0x41E0000000000000ull) return INT32_MIN;
    // The float32 is in [0.5, 2^31): its integer part and fraction, then ties to even.
    int32_t sh = 52 - ((int32_t) (a >> 52) - 1023); // 22 to 53
    uint64_t m = (a & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull;
    uint64_t ip = m >> sh;
    uint64_t fr = m & ((1ull << sh) - 1);
    uint64_t half = 1ull << (sh - 1);
    if (fr > half || (fr == half && (ip & 1))) ip++;
    int32_t v = (int32_t) ip;
    return (b >> 63) ? -v : v;
}

// COLFLIP_ALARM_VALUE's target.
__attribute__((noinline, used, section(".text.late.alarm14"))) int32_t Colflip14_alarmValue(RValue val) {
    if (!Colflip_gm14) return RValue_toInt32(val);
    return Colflip14_alarmRound(RValue_toReal(val));
}
#endif /* ALARM_GM14_DEFINE */

#endif /* _BS_ALARM_GM14_H_ */
