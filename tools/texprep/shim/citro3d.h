// Host stand-in for <citro3d.h> (texprep): the texture formats ctr_texture_pack.c names, with citro3d's values.
#ifndef TEXPREP_SHIM_CITRO3D_H
#define TEXPREP_SHIM_CITRO3D_H

typedef enum {
    GPU_RGBA8 = 0x0,
    GPU_RGB8 = 0x1,
    GPU_RGBA5551 = 0x2,
    GPU_RGB565 = 0x3,
    GPU_RGBA4 = 0x4,
    GPU_LA8 = 0x5,
    GPU_HILO8 = 0x6,
    GPU_L8 = 0x7,
    GPU_A8 = 0x8,
    GPU_LA4 = 0x9,
    GPU_L4 = 0xA,
    GPU_A4 = 0xB,
    GPU_ETC1 = 0xC,
    GPU_ETC1A4 = 0xD,
} GPU_TEXCOLOR;

#endif
