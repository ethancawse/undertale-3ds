#ifndef _BS_BINARY_READER_H_
#define _BS_BINARY_READER_H_

#include "common.h"
#include <stdint.h>
#include <stddef.h>
#include "stdio_compat.h"
#include <stdbool.h>

typedef struct {
    FILE* file;
    size_t fileSize;

    // When non-null, reads come from this memory buffer instead of the FILE*
    // bufferBase is the absolute file offset that buffer[0] corresponds to
    uint8_t* buffer;
    size_t bufferBase;
    size_t bufferSize;
    size_t bufferPos; // current read position relative to bufferBase

    // When non-null (and buffer is null), FILE* reads go through this window: a block of the file read in one go,
    // so seeking around inside it costs no file access. windowPos is the current absolute read position.
    uint8_t* window;
    size_t windowCapacity;
    size_t windowBase;
    size_t windowLength;
    size_t windowPos;
} BinaryReader;

BinaryReader BinaryReader_create(FILE* file, size_t fileSize);

// Sets a memory buffer for bulk chunk reading
// All subsequent reads will come from this buffer until it is cleared
// baseOffset is the absolute file offset that buffer[0] corresponds to
void BinaryReader_setBuffer(BinaryReader* reader, uint8_t* buffer, size_t baseOffset, size_t size);

// Clears the memory buffer, reverting to FILE*-based reads
void BinaryReader_clearBuffer(BinaryReader* reader);

// Reads from the FILE* in blocks of up to 'capacity' bytes from here on, starting at the current file position.
// For parsing that seeks back and forth over a small area of a file read lazily (a room's payload): with a buffered
// FILE*, every seek can throw the stdio buffer away and read it again. The file position afterwards is undefined.
void BinaryReader_beginWindow(BinaryReader* reader, size_t capacity);
void BinaryReader_endWindow(BinaryReader* reader);

uint8_t BinaryReader_readUint8(BinaryReader* reader);
int16_t BinaryReader_readInt16(BinaryReader* reader);
uint16_t BinaryReader_readUint16(BinaryReader* reader);
int32_t BinaryReader_readInt32(BinaryReader* reader);
uint32_t BinaryReader_readUint32(BinaryReader* reader);
float BinaryReader_readFloat32(BinaryReader* reader);
uint64_t BinaryReader_readUint64(BinaryReader* reader);
int64_t BinaryReader_readInt64(BinaryReader* reader);
bool BinaryReader_readBool32(BinaryReader* reader);

// Copies 'count' bytes from the current position into 'dest'.
void BinaryReader_readBytes(BinaryReader* reader, void* dest, size_t count);

// Reads 'count' bytes from position 'offset' into a newly allocated buffer.
// Caller must free the returned buffer.
uint8_t* BinaryReader_readBytesAt(BinaryReader* reader, size_t offset, size_t count);

void BinaryReader_skip(BinaryReader* reader, size_t bytes);
void BinaryReader_seek(BinaryReader* reader, size_t position);
size_t BinaryReader_getPosition(BinaryReader* reader);

#endif /* _BS_BINARY_READER_H_ */
