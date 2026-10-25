#ifndef _BS_VM_SURFACE_READERS_H_
#define _BS_VM_SURFACE_READERS_H_

#include "vm.h"

// Kept out of vm.h, which the generated (AOT) code includes: a change there rebuilds all of it.

// Per object index (dataWin->objt.count entries, malloc'd: the caller frees it): whether the object's code, a script
// it calls or a parent's code reads surface pixels back to the CPU or saves them to a file (surface_getpixel,
// buffer_get_surface, surface_save, screen_save and the like). *count gets how many objects do. Reads all the
// bytecode, so call it before DataWin_moveBytecodeToFile.
bool* VM_findSurfaceReaders(VMContext* ctx, int32_t* count);

#endif /* _BS_VM_SURFACE_READERS_H_ */
