// Host-testable decisions for the opt-in R2 + R3a prototype.
#ifndef CTRPORT_CTR_PACER_POLICY_H
#define CTRPORT_CTR_PACER_POLICY_H
#include <stdbool.h>
#include <stdint.h>

#define PACER_RECOVERY_MARGIN_MS 4u
#ifndef PACER_RECOVERY_LENGTH_FRAMES
#define PACER_RECOVERY_LENGTH_FRAMES 90u
#endif

static inline bool pacerRecoveryHasMargin(uint64_t done, uint64_t budget, uint64_t ticksPerMs) {
    uint64_t margin = PACER_RECOVERY_MARGIN_MS * ticksPerMs;
    return budget >= margin && done <= budget - margin;
}

static inline bool pacerCanReactivate(bool roomNeededDelay, uint32_t delay, bool late,
                                     uint32_t slotLength, bool slotLate) {
    return roomNeededDelay && delay == 0 && late && slotLength >= 2 && !slotLate;
}
#endif
