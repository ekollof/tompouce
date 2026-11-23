#ifndef STRETCH_REAL_FFT_SIMD_H
#define STRETCH_REAL_FFT_SIMD_H
#define STRETCH_REAL_FFT_SIZE 2048
/* fft.js real-transform layout: interleaved complex output, 1024-entry
   table mapping the two half-size inputs, and 1024 twiddles. */
void stretch_real_fft_c(double *out, const float *input,
    const double *twiddle, const unsigned *real_reverse);
#if defined(__x86_64__) || defined(__i386__)
void stretch_real_fft_avx2(double *out, const float *input,
    const double *twiddle, const unsigned *real_reverse);
#endif
void stretch_real_fft(double *out, const float *input,
    const double *twiddle, const unsigned *real_reverse);
#endif
