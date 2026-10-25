// Link this bridge after the original system libraries: its new RomFS reference
// must not extract existing libctru UTF helpers earlier than the frozen build.
#include <3ds.h>

__attribute__((noinline, section(".text.late.relprep")))
Result CtrLegal_mountSelf(const char* name) {
    return romfsMountSelf(name);
}
