// PacketVideo's C++ headers are compiled as C99 here. Their plain inline
// definitions need one external definition when Debug does not inline calls.
// A non-inline declaration in this TU makes the header's bodies external C99
// definitions. Release stays empty, preserving its decoder code and layout.
#ifndef NDEBUG
#include "getbits.h"

extern UInt32 getbits(const UInt neededBits, BITS* input);
extern UInt get1bits(BITS* input);
extern UInt get9_n_lessbits(const UInt neededBits, BITS* input);
extern UInt32 get17_n_lessbits(const UInt neededBits, BITS* input);
#endif
