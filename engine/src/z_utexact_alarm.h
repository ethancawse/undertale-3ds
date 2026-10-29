// UT-040: UNDERTALE.exe 1.08 HandleAlarm (0x4bf292..0x4bf2c9) decrements nonnegative alarms,
// dispatches at zero, and leaves zero visible until the next eligible countdown visit. An assigned zero
// counts down to -1 without dispatch. The 3DS UNDERTALE build has only WAD16; other builds keep their rule.
#ifndef _BS_UTEXACT_ALARM_H_
#define _BS_UTEXACT_ALARM_H_

#if defined(ENABLE_WAD16) && !defined(ENABLE_WAD17)
#define UTEXACT_ALARM_ACTIVE(v) ((v) >= 0)
#define UTEXACT_ALARM_TICK(inst, idx, bit) do { \
    (inst)->alarm[idx]--; \
    if ((inst)->alarm[idx] < 0) (inst)->activeAlarmMask &= (uint16_t) ~(bit); \
} while (0)
#define UTEXACT_ALARM_FIRE(inst, idx, bit) ((void) 0)
#else
#define UTEXACT_ALARM_ACTIVE(v) ((v) > 0)
#define UTEXACT_ALARM_TICK(inst, idx, bit) ((inst)->alarm[idx]--)
#define UTEXACT_ALARM_FIRE(inst, idx, bit) do { \
    (inst)->alarm[idx] = -1; \
    (inst)->activeAlarmMask &= (uint16_t) ~(bit); \
} while (0)
#endif

#endif
