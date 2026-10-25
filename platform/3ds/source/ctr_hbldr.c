#include "ctr_hbldr.h"

#include <3ds.h>
#include <string.h>

#include "ctr_lab_proto.h"

bool CtrHbldr_fileLoadable(const char* sdPath) {
    if (sdPath == NULL || sdPath[0] != '/') return false;
    Handle file = 0;
    Result rc = FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, sdPath), FS_OPEN_READ, 0);
    if (R_FAILED(rc)) return false;
    u64 size = 0;
    u8 header[64];
    u32 got = 0;
    bool ok = R_SUCCEEDED(FSFILE_GetSize(file, &size)) && R_SUCCEEDED(FSFILE_Read(file, &got, 0, header, sizeof(header))) &&
              LabProto_check3dsx(header, got, size);
    FSFILE_Close(file);
    return ok;
}

// The argument block, static so an exception handler's small stack needn't hold it. Only one thread at a time
// leaves (ctr_lab.c), and the lab host has one.
static u8 argvBlock[LAB_ARGV_BUFFER_SIZE] __attribute__((aligned(4)));

const char* CtrHbldr_setNext(const char* sdPath, const char* const* args, int count) {
    if (!CtrHbldr_fileLoadable(sdPath)) return "not a loadable .3dsx";
    if (LabProto_packArgv(argvBlock, args, count) == 0) return "arguments too long";
    Handle port = 0;
    if (R_FAILED(svcConnectToPort(&port, "hb:ldr"))) return "no hb:ldr (not Luma3DS)";
    const char* problem = NULL;
    u32* cmdbuf = getThreadCommandBuffer();
    // SetTarget: the path as UTF-8 in static buffer 0.
    cmdbuf[0] = IPC_MakeHeader(2, 0, 2);
    cmdbuf[1] = IPC_Desc_StaticBuffer(strlen(sdPath) + 1, 0);
    cmdbuf[2] = (u32) sdPath;
    Result rc = svcSendSyncRequest(port);
    if (R_SUCCEEDED(rc)) rc = (Result) cmdbuf[1];
    if (R_FAILED(rc)) {
        problem = "hb:ldr refused the path";
    } else {
        // SetArgv: the whole block in static buffer 1 (the loader copies 0x400 bytes whatever the size says).
        cmdbuf[0] = IPC_MakeHeader(3, 0, 2);
        cmdbuf[1] = IPC_Desc_StaticBuffer(sizeof(argvBlock), 1);
        cmdbuf[2] = (u32) argvBlock;
        rc = svcSendSyncRequest(port);
        if (R_SUCCEEDED(rc)) rc = (Result) cmdbuf[1];
        if (R_FAILED(rc)) problem = "hb:ldr refused the arguments";
    }
    svcCloseHandle(port);
    return problem;
}
