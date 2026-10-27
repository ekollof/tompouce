#include "signal_wave.h"
#include <math.h>
#include <stdint.h>

static double js_mod(double x, double y) {
    if (!isfinite(x) || !isfinite(y) || y == 0) return NAN;
    return fmod(x, y);
}

/* JavaScript ToInt32 shifts. The seed input fits in int32. */
static int32_t xorwise(int32_t x) {
    uint32_t ux = (uint32_t)x;
    uint32_t a = (ux << 13) ^ ux;
    int32_t sa = (int32_t)a;
    uint32_t b = (uint32_t)((sa >> 17) ^ sa);
    return (int32_t)((b << 5) ^ b);
}

static int32_t time_to_int_seed(double x) {
    if (!isfinite(x)) return 0;
    double q = x / 300.0;
    double scaled = trunc((q - trunc(q)) * 536870912.0);
    return xorwise((int32_t)scaled);
}

static double int_seed_to_rand(int32_t seed) {
    return js_mod((double)seed, 536870912.0) / 536870912.0;
}

static double rand_at(double t, double seed) {
    double value = int_seed_to_rand(time_to_int_seed(t + seed));
    return fabs(value);
}

static double perlin_at(double t, double seed) {
    if (!isfinite(t)) return NAN;
    double ta = floor(t);
    double x = t - ta;
    double smooth = 6.0 * pow(x, 5.0) - 15.0 * pow(x, 4.0) + 10.0 * pow(x, 3.0);
    double left = rand_at(ta, seed);
    double right = rand_at(ta + 1.0, seed);
    return left + smooth * (right - left);
}

static double berlin_at(double t, double seed) {
    if (!isfinite(t)) return NAN;
    double prev = floor(t);
    double bottom = rand_at(prev, seed);
    double height = rand_at(prev + 1.0, seed);
    return (bottom + (t - prev) * height) / 2.0;
}

static double sample_one(int kind, double t, double seed) {
    switch (kind) {
    case SIGNAL_SAW: return js_mod(t, 1.0);
    case SIGNAL_ISAW: return 1.0 - js_mod(t, 1.0);
    case SIGNAL_SINE2: return sin(0x1.921fb54442d18p+2 * t);
    case SIGNAL_SQUARE: return floor(js_mod(t * 2.0, 2.0));
    case SIGNAL_PERLIN: return perlin_at(t, seed);
    case SIGNAL_BERLIN: return berlin_at(t, seed);
    case SIGNAL_RAND: return rand_at(t, seed);
    default: return NAN;
    }
}

int signal_wave_fill(const int *kind, const double *time, int count, double seed, double *out) {
    if (!kind || !time || !out || count < 0) return -1;
    for (int i = 0; i < count; i++) {
        if (kind[i] < 0 || kind[i] >= SIGNAL_WAVES) return -1;
        out[i] = sample_one(kind[i], time[i], seed);
    }
    return 0;
}
