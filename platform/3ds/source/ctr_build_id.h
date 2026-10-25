#pragma once

// The build ID ("<commit>[-dirty][+aot...]", platform/3ds/build_id.cmake) for the log header, the boot screen and the
// performance test. It lives in one fixed-size array (main.c) rather than in a string literal, so a "-dirty" build, a
// longer hash or another AOT tag changes its bytes but never its size: every later .rodata symbol keeps its address
// (docs/3ds-hardware.md, "The build ID's length moved .rodata until it got a fixed size"). The array starts with a
// NUL so that tools which find the ID in a .3dsx by its terminators (tools/remote/queue_lint.py) still do.
#include "ctr_release_contract.h"
#define CTRPORT_BUILD_ID_SIZE CTR_RELEASE_BUILD_ID_SIZE
extern const char ctrportBuildId[CTRPORT_BUILD_ID_SIZE];
#define CTRPORT_COMMIT (ctrportBuildId + 1)
