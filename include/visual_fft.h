#ifndef VISUAL_FFT_H
#define VISUAL_FFT_H
/* In-place radix-2 FFT for `n` (power of two, at most 4096) real-input
   spectra. `imag` starts zero; implements the visualizer's transform order. */
void visual_fft_c(double *re, double *imag, int n);
#if defined(__x86_64__) || defined(__i386__)
void visual_fft_avx2(double *re, double *imag, int n);
#endif
void visual_fft(double *re, double *imag, int n);
#endif
