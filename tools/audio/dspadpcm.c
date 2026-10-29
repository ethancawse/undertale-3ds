/* Offline DSP-ADPCM encoder. No Nintendo SDK or game-derived constants.
 * Eight per-channel predictors are trained by least-squares clustering.
 * Each 14-sample frame searches predictor and scale using reconstructed history.
 * An independent Python decoder checks block histories; a THP decoder checks
 * the byte coding with integer predictors (THP truncates fractional predictors).
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define API __declspec(dllexport)
#else
#define API __attribute__((visibility("default")))
#endif

static int clip(int x, int low, int high) { return x < low ? low : x > high ? high : x; }
static int rounded(double x) { return (int) (x >= 0 ? floor(x + 0.5) : ceil(x - 0.5)); }
static int sample(int64_t x) {
    /* Mathematical floor division, including negative values, then saturate. */
    x += 1024;
    int64_t v = x >= 0 ? x / 2048 : -((-x + 2047) / 2048);
    return v < -32768 ? -32768 : v > 32767 ? 32767 : (int) v;
}

static void stable(double *a, double *b) {
    if (*b < -0.98) *b = -0.98;
    if (*b > 0.98) *b = 0.98;
    double bound = 0.999 * (1.0 - *b);
    if (*a > bound) *a = bound;
    if (*a < -bound) *a = -bound;
}

API int dtad_train(const int16_t *pcm, uint32_t frames, uint32_t channels,
                  uint32_t channel, int16_t *coefs) {
    if (!pcm || !coefs || !frames || channels < 1 || channels > 2 || channel >= channels) return 0;
    /* Bounded sample of local predictors, independent for each source channel. */
    double points[2048][2];
    uint32_t count = 0, windows = (frames + 27u) / 28u;
    uint32_t stride = windows / 2048u + 1u;
    for (uint32_t w = 0; w < windows && count < 2048; w += stride) {
        double xx = 0, xy = 0, yy = 0, tx = 0, ty = 0;
        uint32_t end = (w + 1u) * 28u;
        if (end > frames) end = frames;
        uint32_t begin = w * 28u;
        if (begin < 2) begin = 2;
        for (uint32_t i = begin; i < end; i++) {
            double t = pcm[i * channels + channel];
            double x = pcm[(i - 1u) * channels + channel];
            double y = pcm[(i - 2u) * channels + channel];
            xx += x*x; xy += x*y; yy += y*y; tx += t*x; ty += t*y;
        }
        double det = xx * yy - xy * xy;
        if (det <= (xx * yy + 1.0) * 1e-8) continue;
        double a = (tx * yy - ty * xy) / det;
        double b = (ty * xx - tx * xy) / det;
        stable(&a, &b);
        points[count][0] = a; points[count++][1] = b;
    }
    double centers[8][2] = {{0,0}, {0.5,0}, {0.9,0}, {1.5,-0.7},
                             {1.8,-0.9}, {1,-0.5}, {-0.5,0}, {0,0.5}};
    /* Keep a zero predictor available for noise/transients. */
    if (count) {
        for (int j = 1; j < 8; j++) {
            centers[j][0] = points[(uint32_t) j * count / 8u][0];
            centers[j][1] = points[(uint32_t) j * count / 8u][1];
        }
        for (int iteration = 0; iteration < 12; iteration++) {
            double sums[8][2] = {{0}};
            uint32_t weights[8] = {0};
            for (uint32_t i = 0; i < count; i++) {
                double best = HUGE_VAL;
                int chosen = 0;
                for (int j = 0; j < 8; j++) {
                    double a = points[i][0] - centers[j][0];
                    double b = points[i][1] - centers[j][1];
                    double error = a*a + b*b;
                    if (error < best) { best = error; chosen = j; }
                }
                sums[chosen][0] += points[i][0];
                sums[chosen][1] += points[i][1];
                weights[chosen]++;
            }
            for (int j = 1; j < 8; j++) if (weights[j]) {
                centers[j][0] = sums[j][0] / weights[j];
                centers[j][1] = sums[j][1] / weights[j];
            }
        }
    }
    for (int j = 0; j < 8; j++) {
        stable(&centers[j][0], &centers[j][1]);
        coefs[j*2] = (int16_t) rounded(centers[j][0] * 2048.0);
        coefs[j*2+1] = (int16_t) rounded(centers[j][1] * 2048.0);
    }
    return 1;
}

API int dtad_encode(const int16_t *pcm, uint32_t frames, uint32_t channels,
                   uint32_t channel, const int16_t *coefs, int16_t *history, uint8_t *out) {
    if (!pcm || !coefs || !history || !out || channels < 1 || channels > 2 || channel >= channels) return 0;
    int h0 = history[0], h1 = history[1];
    for (uint32_t at = 0; at < frames; at += 14u) {
        uint32_t n = frames - at < 14u ? frames - at : 14u;
        uint64_t best = UINT64_MAX;
        int besth0 = h0, besth1 = h1;
        uint8_t bestframe[8] = {0};
        for (int pred = 0; pred < 8; pred++) {
            int a = coefs[pred*2], b = coefs[pred*2+1];
            for (int shift = 0; shift <= 15; shift++) {
                int x0 = h0, x1 = h1;
                uint64_t error = 0;
                uint8_t encoded[8] = {0};
                encoded[0] = (uint8_t) ((pred << 4) | shift);
                for (uint32_t i = 0; i < n; i++) {
                    int source = pcm[(at + i) * channels + channel];
                    int64_t prediction = (int64_t) a*x0 + (int64_t) b*x1;
                    double residual = ((double) source * 2048.0 - (double) prediction) / (2048.0 * (1 << shift));
                    int q = clip(rounded(residual), -8, 7);
                    int decoded = sample((int64_t) q * (1 << shift) * 2048 + prediction);
                    int64_t delta = source - decoded;
                    error += (uint64_t) (delta * delta);
                    encoded[1u + i/2u] |= (uint8_t) ((q & 15) << (i % 2u ? 0 : 4));
                    x1 = x0; x0 = decoded;
                    if (error >= best) break;
                }
                if (error < best) {
                    best = error; besth0 = x0; besth1 = x1;
                    memcpy(bestframe, encoded, sizeof(bestframe));
                }
            }
        }
        memcpy(out + (at / 14u) * 8u, bestframe, sizeof(bestframe));
        h0 = besth0; h1 = besth1;
    }
    history[0] = (int16_t) h0; history[1] = (int16_t) h1;
    return 1;
}
