// Room-entry attribution trace (roomload), profile builds only: see z_roomload_trace.c.
#ifndef Z_ROOMLOAD_TRACE_H
#define Z_ROOMLOAD_TRACE_H

#include <stdint.h>

#include "ctr_renderer.h"
#include "runner.h"

void RoomloadTrace_frame(Runner* runner, Renderer* renderer, uint64_t frameStart, double stepMs, double drawMs);

#endif
