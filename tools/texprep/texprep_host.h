// texprep: prepares a data.win's texture cache folder on a PC with the console's own conversion code.
//
// The console's sources (platform/3ds/source/ctr_texture_pack.c and the decoders and LZ codec it calls) are
// compiled unchanged for the host. shim/ stands in for libctru and citro3d, host_platform.c implements the few
// console services they call, and texprep.c drives the console's foreground worker exactly as the "Preparing
// textures" screen does (ctr_texture_pack_screen.c). The output is the folder the console would have written.
//
// The worker's thread count only changes which thread packs which page; each page file is built by one thread
// from that page's bytes alone, so the files are the same for any count.
#ifndef TEXPREP_HOST_H
#define TEXPREP_HOST_H

#include <stdbool.h>

extern bool texprepVerbose;

#endif
