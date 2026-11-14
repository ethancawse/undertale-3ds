// Dedicated-shop spatial crop. S3 resolves the names-only room records;
// no game room indices or object routing are compiled into this consumer.
#ifndef CTR_SHOPSPLIT_H
#define CTR_SHOPSPLIT_H

#include <stdbool.h>
#include "ctr_display.h"

// S3 mode 0 remains the existing battle placement. Mode 1 crops the
// completed shop application at native row 120, keeping both halves by the
// hinge. The scene has band 120, lift 0, split true and no class entries.
enum { CTR_LAYOUT_MODE_SHOP = 1, CTR_SHOPSPLIT_BAND = 120 };

struct Renderer;
bool CtrShopsplit_admitted(struct Renderer* renderer, const CtrDisplaySettings* settings);

#endif
