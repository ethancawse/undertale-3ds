// Luma3DS's hb:ldr service: which .3dsx the Homebrew Launcher's title loads the next time it starts.
//
// Every .3dsx runs inside a title Luma3DS lends it (the Homebrew Launcher's, or the one Rosalina's homebrew mode
// takes over), and libctru has an app that exits normally restart that title ("chainload to self"). When the title
// starts, Luma3DS's loader loads the .3dsx hb:ldr was last told about, once; then /boot.3dsx (the Homebrew Launcher)
// again. 3ds-hbmenu launches everything this way (source/loaders/rosalina.c). Lab mode (ctr_lab.h) and the lab host
// (platform/3ds/labhost/) use it to go from one .3dsx to the next without the menu.
//
// Luma3DS's loader stops the whole console (a panic, "assertSuccess") when the file it was told about can't be
// loaded, so CtrHbldr_setNext checks the file first. Nothing here allocates or uses stdio: lab mode calls it from
// an exception handler.
#ifndef CTRPORT_CTR_HBLDR_H
#define CTRPORT_CTR_HBLDR_H

#include <stdbool.h>
#include <stddef.h>

// Checks that sdPath (an SD card path, "/3ds/x.3dsx") is a .3dsx the loader can take, then has hb:ldr load it, with
// these arguments (argv[0] first, as the app will see them), the next time the launcher title starts. Returns
// nullptr when done, else what went wrong (a string literal). Without Luma3DS (an emulator) it fails harmlessly.
const char* CtrHbldr_setNext(const char* sdPath, const char* const* args, int count);

// Whether sdPath holds a .3dsx the loader can take (what CtrHbldr_setNext checks first).
bool CtrHbldr_fileLoadable(const char* sdPath);

#endif
