#ifndef OSCILLATOR_SIMD_H
#define OSCILLATOR_SIMD_H
/* Four independent polyBLEP saw oscillators. */
void dsp_saw4_c(const double phase[4], const double freq[4], double output[4]);
#if defined(__x86_64__) || defined(__i386__)
void dsp_saw4_avx2(const double phase[4], const double freq[4],
    double output[4]);
#endif
void dsp_saw4(const double phase[4], const double freq[4], double output[4]);
#endif
