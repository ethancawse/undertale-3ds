// PWM calibration layout and transfer function documented by Luma3DS's public
// sysmodules/rosalina/source/luminance.c. No guessed calibration or preset.
#ifndef CTR_DIMLIGHT_LOGIC_H
#define CTR_DIMLIGHT_LOGIC_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    float coeffs[3][3];
    uint8_t numLevels, unused;
    uint16_t levels[7], maximum, minimum;
} DimCalibration;
_Static_assert(sizeof(DimCalibration) == 56, "public calibration layout");

static inline __attribute__((always_inline)) bool dimDuty(const DimCalibration* c, uint32_t pwm, uint32_t raw, uint32_t* duty) {
    double a = c->coeffs[0][0], b = c->coeffs[0][1], d = c->coeffs[0][2];
    if (!c->maximum || c->minimum > c->maximum || !isfinite(a) || !isfinite(b) || !isfinite(d) || a < 0 || b <= 0)
        return false;
    double y = a * raw * raw + b * raw + d;
    if (y < c->minimum) y = c->minimum;
    uint32_t denominator = ((pwm & 0x10000u) ? pwm & 0x3ffu : 511u) + 1;
    y = y * denominator / c->maximum;
    if (!isfinite(y) || y < 0 || y > 65535) return false;
    *duty = (uint32_t) (y + 0.5);
    return true;
}

// Search calibration numbers, never the light. An exact readback match is the
// gate for touching brightness. The known preset range is 16..172 luminance units.
static inline __attribute__((always_inline)) bool dimRestoreRaw(const DimCalibration* c, uint32_t pwm, uint32_t saved, uint32_t* raw) {
    for (uint32_t n = 16; n <= 172; ++n) {
        uint32_t duty;
        if (!dimDuty(c, pwm, n, &duty)) return false;
        if (duty == saved) { *raw = n; return true; }
    }
    return false;
}
#endif
