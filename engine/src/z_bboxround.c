// Collision_computeBBox's rounding on the 3DS (simplefix, 2026-10-28).
//
// In collision compatibility mode (DELTARUNE sets the option; UNDERTALE is older than GameMaker 2022.1, so it always
// runs it) every bounding box rounds its four edges with GMLReal_bankersRound: a call to newlib's floor and four or
// five FPSCR flag reads per edge, about 39,000 boxes in one frame of DELTARUNE Chapter 3's board pans. This returns
// the same bits more cheaply. Collision_bboxRound is a leaf: for 1 <= |v| < 2^31 it truncates with the VFP (vcvt
// without R, which ignores FPSCR's rounding mode), takes the fraction's size exactly and settles a whole number with
// one compare, a fraction with two or three. Every other input (the zeros, |v| < 1, |v| >= 2^31, infinities, NaNs)
// goes to bboxRoundBits, which works from the double's bits: 0.5 < |v| < 1 gives +-1.0; -0.0 and +0.0 come back as
// they are; another negative input that rounds to zero returns -1 + 1 added at run time, as the old path does, so
// that zero's sign follows the thread's rounding mode as before; other zeros are +0.0 both ways; |v| >= 1 (from the
// front, only |v| >= 2^31 - 1), infinities and NaNs take the old path. No step depends on FPSCR's rounding
// mode or flush-to-zero except where the old path's does too. The engine never writes FPSCR; Azahar starts the main
// thread at 0x03000010 (nearest, flush-to-zero, default NaN) on Old and New. Proof: a host harness (every
// float32 value and 15 billion doubles under the four rounding modes, flush to zero on and off, against newlib's
// floor) and the same table run through the VFP on Azahar, as found and in each mode with flush to zero on and off.
//
// collision.h calls it from Collision_computeBBox in the translation units that define CTRPORT_BBOX_ROUND (runner.c
// and vm_builtins.c, CMakeLists.txt), whose out-of-line copies of that function keep their slots
// (runner.c's and vm_builtins.c's ends); elsewhere it is GMLReal_bankersRound itself.
#include "real_type.h"

#include <stdint.h>
#include <string.h>

#if defined(__3DS__) && !defined(USE_FLOAT_REALS)
// The bit path: the zeros, |v| < 1, |v| >= 2^31 (the conversion below saturates), infinities and NaNs.
__attribute__((noinline, section(".text.bboxround_late"))) static GMLReal bboxRoundBits(GMLReal v) {
    uint64_t b;
    memcpy(&b, &v, sizeof(b));
    uint32_t e = (uint32_t) (b >> 52) & 0x7FF;
    if (e >= 1023) return GMLReal_bankersRound(v); // |v| >= 1, inf, NaN (the front sends only |v| >= 2^31 - 1 here)
    if (e == 1022 && (b & 0xFFFFFFFFFFFFFull) != 0) return (b >> 63) != 0 ? -1.0 : 1.0; // 0.5 < |v| < 1
    if ((b << 1) == 0) return v;
    if ((b >> 63) == 0) return 0.0;
    GMLReal m1 = -1.0;
    __asm__("" : "+w"(m1)); // the add happens at run time, under FPSCR
    return m1 + 1.0;
}

// |v| >= 1 below 2^31: t = v toward zero (vcvt without R ignores FPSCR's rounding mode; it saturates, NaN gives 0),
// back = t. v - back is v's fraction, exact (no rounding, a normal number or zero, so no flush either), and back +/- 1
// are whole numbers below 2^53, exact too: no step depends on FPSCR. The result is never zero, so no sign question.
__attribute__((section(".text.bboxround_late"))) GMLReal Collision_bboxRound(GMLReal v) {
    float ts;
    GMLReal back;
    __asm__("vcvt.s32.f64 %0, %P1" : "=t"(ts) : "w"(v));
    __asm__("vcvt.f64.s32 %P0, %1" : "=w"(back) : "t"(ts));
    int32_t t;
    memcpy(&t, &ts, sizeof(t));
    if (t == 0 || (uint32_t) t - 0x7FFFFFFFu <= 1u) return bboxRoundBits(v); // 0, INT32_MAX or INT32_MIN
    GMLReal d = __builtin_fabs(v - back); // the fraction's size (the fraction has v's sign)
    if (d < 0.5) return back;             // whole numbers and most fractions: one compare
    if (d > 0.5 || (t & 1) != 0) return t < 0 ? back - 1.0 : back + 1.0; // away from zero, or a tie to even
    return back;
}
#else
GMLReal Collision_bboxRound(GMLReal v);
GMLReal Collision_bboxRound(GMLReal v) {
    return GMLReal_bankersRound(v);
}
#endif
