#pragma once

#include "runner.h"

// Test clocks only: null keeps the normal AAC / active-play clock. Configure
// before the runner's first room; each opened video captures this runner.
void CtrVideo_setFrameClock(Runner* runner);

// Test only: exercise the video's APT clock hooks and audio suspension from
// the game thread, outside a GPU frame. Enabled by --video-suspend F:N.
void CtrVideo_debugSuspend(Runner* runner, unsigned seconds);
