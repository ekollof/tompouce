#ifndef VOCODE_TAILS_SIMD_H
#define VOCODE_TAILS_SIMD_H
#include "vocoder.h"
#if (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__)) && !defined(TOMPOUCE_DISABLE_SIMD)
#define VOCODE_TAILS_HAVE_AVX2 1
#else
#define VOCODE_TAILS_HAVE_AVX2 0
#endif
/* Elementwise vocoder tail loops: Hann windowing in, bin magnitudes,
   conjugate mirror, and windowed overlap-add out. Each output element is
   computed with the scalar operation order, so the C and AVX2 backends
   agree bit for bit. Sizes are fixed: VOCODE_FFT samples, with magnitudes
   over VOCODE_FFT/2+1 bins. */
void vocoder_window_c(float *windowed, const float *input, const float *hann);
#if VOCODE_TAILS_HAVE_AVX2
void vocoder_window_avx2(float *windowed, const float *input,
    const float *hann);
#endif
void vocoder_window(float *windowed, const float *input, const float *hann);
void vocoder_magnitudes_c(float *magnitudes, const VocoderComplex *freq);
#if VOCODE_TAILS_HAVE_AVX2
void vocoder_magnitudes_avx2(float *magnitudes, const VocoderComplex *freq);
#endif
void vocoder_magnitudes(float *magnitudes, const VocoderComplex *freq);
void vocoder_mirror_c(VocoderComplex *shifted);
#if VOCODE_TAILS_HAVE_AVX2
void vocoder_mirror_avx2(VocoderComplex *shifted);
#endif
void vocoder_mirror(VocoderComplex *shifted);
void vocoder_ola_c(float *sum, const VocoderComplex *shifted,
    const float *hann);
#if VOCODE_TAILS_HAVE_AVX2
void vocoder_ola_avx2(float *sum, const VocoderComplex *shifted,
    const float *hann);
#endif
void vocoder_ola(float *sum, const VocoderComplex *shifted,
    const float *hann);
#endif
