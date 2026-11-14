// Where the battle is drawn on the bottom screen this frame, in room coordinates (battletouch design, section 2).
// battletouch defines CtrLayout_bottomView weak, for the game on the bottom screen; battlesplit's strong definition
// replaces it and must keep that case. false: the battle isn't on the bottom screen, so the stylus does nothing.
#ifndef CTR_BOTTOM_VIEW_H
#define CTR_BOTTOM_VIEW_H

#include <stdbool.h>

typedef struct { float roomX0, roomY0, roomPerPx; } CtrBottomView;
struct Runner;
bool CtrLayout_bottomView(struct Runner* runner, CtrBottomView* out);

#endif
