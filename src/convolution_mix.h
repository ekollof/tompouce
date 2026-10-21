#ifndef CONVOLUTION_MIX_H
#define CONVOLUTION_MIX_H
#include "convolution.h"
typedef void (*ConvolutionAccumulate)(ConvComplex *,const ConvComplex *,const ConvComplex *,unsigned);
/* Initialize off the callback. Until initialized, selection is portable C.
   Selection is immutable after initialization and safe for concurrent readers. */
void convolution_simd_init(void);
const char *convolution_simd_name(void);
ConvolutionAccumulate convolution_accumulator(void);
#endif
