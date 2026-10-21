#ifndef CONVOLUTION_MIX_H
#define CONVOLUTION_MIX_H
#include "convolution.h"
typedef void (*ConvolutionAccumulate)(ConvComplex *,const ConvComplex *,const ConvComplex *,unsigned);
/* Initialize off the callback. Until initialized, selection is portable C.
   Selection is immutable after initialization and safe for concurrent readers. */
void convolution_simd_init(void);
const char *convolution_simd_name(void);
typedef struct {
    const char *kernel, *policy, *available;
    int fallback;
} ConvolutionSimdStats;
/* Cached startup diagnostics: no detection, allocation or initialization. */
ConvolutionSimdStats convolution_simd_stats(void);
ConvolutionAccumulate convolution_accumulator(void);
#endif
