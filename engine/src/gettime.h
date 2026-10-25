#ifndef _BS_GETTIME_H_
#define _BS_GETTIME_H_

#include <stdbool.h>
#include <stdint.h>

uint64_t nowNanos(void);

// Seconds since the Unix epoch, for the date and time builtins (current_second, date_current_datetime...).
int64_t nowEpochSeconds(void);

// Deterministic clock for regression runs (--state-hash, statehash.txt on the 3DS): once enabled, nowNanos() returns a
// virtual time that only moves when GetTime_advanceVirtualClock is called, so current_time/get_timer/delta_time repeat
// exactly between runs, and nowEpochSeconds() a fixed date moved on by that virtual time, so the date builtins do too.
void GetTime_enableVirtualClock(void);
void GetTime_advanceVirtualClock(uint64_t nanos);
// True once the virtual clock is on: anything else that reads real time (audio track positions) should follow it.
bool GetTime_virtualClockOn(void);

#endif /* _BS_GETTIME_H_ */
