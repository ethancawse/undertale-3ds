// Analytic shader parameters, independent of any room, object or texture.
// Keep these shared by the mesh and cached-mask paths. The generated mask
// checks the rational constants so changing the shader cannot use stale data.
#ifndef CTRPORT_CTR_FOUNTAIN_SHAPE_H
#define CTRPORT_CTR_FOUNTAIN_SHAPE_H
#define FOUNTAIN_EDGE_NUM 5
#define FOUNTAIN_EDGE_DEN 1000
#define FOUNTAIN_STRIP_NUM 101
#define FOUNTAIN_STRIP_DEN 10000
#define FOUNTAIN_MASK_W 128
#define FOUNTAIN_MASK_H 256
#define FOUNTAIN_EDGE ((float) FOUNTAIN_EDGE_NUM / FOUNTAIN_EDGE_DEN)
#define FOUNTAIN_STRIP ((float) FOUNTAIN_STRIP_NUM / FOUNTAIN_STRIP_DEN)
#define FOUNTAIN_WAVE_FREQUENCY 6.0f
#endif
