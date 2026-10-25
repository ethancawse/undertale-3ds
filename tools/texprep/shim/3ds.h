// Host stand-in for libctru's <3ds.h>, for texprep (tools/texprep/texprep_host.h).
//
// Only what the console's texture preparation sources use (ctr_texture_pack.c,
// ctr_qoi.c, ctr_cache_publish.c). Threads and locks are real (pthreads); the
// console-only services report failure or a neutral value, so the device code
// takes its ordinary stdio paths. Nothing here changes what the device code
// computes: decoding, format choice, tiling, LZ and the file layout run as on
// the console.
#ifndef TEXPREP_SHIM_3DS_H
#define TEXPREP_SHIM_3DS_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef s32 Result;

#ifndef U64_MAX
#define U64_MAX UINT64_MAX
#endif

#define R_SUCCEEDED(res) ((res) >= 0)
#define R_FAILED(res) ((res) < 0)
#define R_SUMMARY(res) (((res) >> 21) & 0x3F)
#define R_DESCRIPTION(res) ((res) & 0x3FF)
#define RS_NOTFOUND 4
#define RD_NOT_FOUND 0x3F0
// Any failure: the host has no console services.
#define TEXPREP_NO_SERVICE ((Result) 0xD8E007F7)

#define SYSCLOCK_ARM11 268111856ull

typedef pthread_mutex_t LightLock;
void LightLock_Init(LightLock* lock);
void LightLock_Lock(LightLock* lock);
int LightLock_TryLock(LightLock* lock);
void LightLock_Unlock(LightLock* lock);

typedef struct TexprepThread* Thread;
typedef void (*ThreadFunc)(void*);
Thread threadCreate(ThreadFunc entry, void* arg, size_t stackSize, int prio, int core, bool detached);
Result threadJoin(Thread thread, u64 timeoutNs);
void threadFree(Thread thread);

void svcSleepThread(s64 ns);
s32 svcGetProcessorID(void);
u64 svcGetSystemTick(void);

Result APT_CheckNew3DS(bool* out);
Result APT_GetAppCpuTimeLimit(u32* percent);
Result APT_SetAppCpuTimeLimit(u32 percent);

typedef u64 FS_Archive;
typedef enum { PATH_INVALID = 0, PATH_EMPTY = 1, PATH_BINARY = 2, PATH_ASCII = 3, PATH_UTF16 = 4 } FS_PathType;
typedef enum { ARCHIVE_SDMC = 9 } FS_ArchiveID;
typedef struct {
    FS_PathType type;
    u32 size;
    const void* data;
} FS_Path;
FS_Path fsMakePath(FS_PathType type, const void* path);
Result FSUSER_OpenArchive(FS_Archive* archive, FS_ArchiveID id, FS_Path path);
Result FSUSER_CloseArchive(FS_Archive archive);
Result FSUSER_DeleteFile(FS_Archive archive, FS_Path path);
Result FSUSER_RenameFile(FS_Archive srcArchive, FS_Path srcPath, FS_Archive dstArchive, FS_Path dstPath);

void* linearAlloc(size_t size);
void linearFree(void* p);
u32 linearSpaceFree(void);
Result GSPGPU_FlushDataCache(const void* addr, u32 size);

#endif
