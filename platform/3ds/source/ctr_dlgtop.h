// S4's public, read-only arbitration query. T1 calls before requesting its loan.
#ifndef CTR_DLGTOP_H
#define CTR_DLGTOP_H
#include <stdbool.h>
struct Renderer;
bool CtrDlgtop_topBusy(struct Renderer* renderer);
#endif
