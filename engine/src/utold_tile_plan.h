/* Ordered legacy-tile source plans. No residency, vertex or GPU state is cached. */
#ifndef UTOLD_TILE_PLAN_API_H
#define UTOLD_TILE_PLAN_API_H
#include <stdint.h>

#if defined(__3DS__)
#define UTP_CODE __attribute__((noinline,section(".text.late.utold")))
#else
#define UTP_CODE
#endif
typedef struct Runner Runner;
void Utp_invalidate(Runner* runner);
void Utp_drawablesChanged(Runner* runner);
void Utp_release(Runner* runner);
void Utp_roomLoaded(Runner* runner);
#endif
