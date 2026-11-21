#ifndef DSP_SUPERSAW_H
#define DSP_SUPERSAW_H
#include "dsp.h"

#if (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__)) && !defined(TOMPOUCE_DISABLE_SIMD)
#define DSP_SUPERSAW_HAVE_AVX2 1
#else
#define DSP_SUPERSAW_HAVE_AVX2 0
#endif

/* Produce one sample from every unison oscillator and advance its phase.
   The caller keeps the original index-order summation. */
void dsp_supersaw_values(DspState *state, double frequency, double *values);
void dsp_supersaw_values_c(DspState *state, double frequency, double *values);
#if DSP_SUPERSAW_HAVE_AVX2
void dsp_supersaw_values_avx2(DspState *state, double frequency, double *values);
#endif

#endif
