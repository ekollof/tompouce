#ifndef STRETCH_FFT_SIMD_H
#define STRETCH_FFT_SIMD_H
#define STRETCH_FFT_SIZE 2048
/* Interleaved complex data, re/im doubles. `twiddle` is 1024 interleaved
   complex values; `reverse_bits` is the 2048-entry bit-reversal table. */
void stretch_fft_c(double *data, int inverse, const double *twiddle,
    const unsigned *reverse_bits);
#if defined(__x86_64__) || defined(__i386__)
void stretch_fft_avx2(double *data, int inverse, const double *twiddle,
    const unsigned *reverse_bits);
#endif
void stretch_fft(double *data, int inverse, const double *twiddle,
    const unsigned *reverse_bits);
#endif
