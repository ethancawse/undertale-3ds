#ifndef UTOLD_TILE_ASSETS_H
#define UTOLD_TILE_ASSETS_H
#include <stdint.h>
/* Optional roomplan source extension. valid=-1 means build at room load.
 * Only byte-matching loaded tile fields can adopt a builder command. */
struct DataWin;
struct Runner;
void UtpAssets_bind(struct Runner* runner,void* commands,uint32_t count);
#endif
