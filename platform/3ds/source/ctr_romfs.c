#include "ctr_benchromfs.h"

static bool mountTried __attribute__((section(".bss.late.benchromfs")));
static Result mountResult __attribute__((section(".bss.late.benchromfs")));

__attribute__((noinline, section(".text.late.benchromfs")))
Result CtrRomfs_init(const char* name) {
    if (!mountTried) {
        mountTried = true;
        mountResult = romfsMountSelf(name);
    }
    return mountResult;
}
