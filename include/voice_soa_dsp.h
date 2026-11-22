#ifndef VOICE_SOA_DSP_H
#define VOICE_SOA_DSP_H
#include "dsp.h"

/* Four independent biquads processed in parallel. Coefficients and state are
   SoA; every lane executes the scalar biquad operation order. */
typedef struct {
    double b0[4], b1[4], b2[4], a1[4], a2[4], z1[4], z2[4];
} DspBiquad4;

#if (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__)) && !defined(TOMPOUCE_DISABLE_SIMD)
#define VOICE_SOA_DSP_HAVE_AVX2 1
#else
#define VOICE_SOA_DSP_HAVE_AVX2 0
#endif

void dsp_biquad4_init(DspBiquad4 *dst, const DspBiquad source[4]);
void dsp_biquad4_process(DspBiquad4 *state, const double *input,
    double *output, unsigned frames);
void dsp_biquad4_process_c(DspBiquad4 *state, const double *input,
    double *output, unsigned frames);
#if VOICE_SOA_DSP_HAVE_AVX2
void dsp_biquad4_process_avx2(DspBiquad4 *state, const double *input,
    double *output, unsigned frames);
#endif

#endif
