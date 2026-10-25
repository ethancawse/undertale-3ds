// Byte-oriented LZ77 codec for the texture cache.
//
// The stream layout follows the LZ4 block format (token with 4-bit literal
// and match lengths, 16-bit match offsets, minimum match of 4 bytes), without
// LZ4's end-of-block restrictions: any sequence may end the stream. It is
// chosen for decode speed on the ARM11, where inflating the same data costs
// several times as much. Decoding checks every bound, so corrupt input fails
// instead of writing out of range.
#ifndef CTRPORT_CTR_LZ_H
#define CTRPORT_CTR_LZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Largest compressed size for srcSize input bytes.
size_t CtrLz_bound(size_t srcSize);

// Compresses src into dst (at least CtrLz_bound(srcSize) bytes) and returns
// the compressed size, or 0 if the match tables (at most 192 KB) can't be
// allocated. `step` is the size of the data's elements (texels: 2 or 4
// bytes, else 1): matches are only looked for where one starts, which is
// faster and loses little. Thread-safe.
size_t CtrLz_compress(const uint8_t* src, size_t srcSize, uint8_t* dst, uint32_t step);

// Decompresses exactly dstSize bytes. Fails if the stream is malformed or does
// not produce exactly dstSize bytes from exactly srcSize bytes.
bool CtrLz_decompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize);

#endif
