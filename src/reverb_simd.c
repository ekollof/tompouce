#include "reverb_simd.h"
#include <math.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static atomic_int backend=-1;

static int use_avx2(void) {
    int cached=atomic_load_explicit(&backend,memory_order_relaxed);
    if (cached<0) {
        cached=0;
        const char *requested=getenv("TOMPOUCE_SIMD");
        __builtin_cpu_init();
        int allow=!requested || !*requested || !strcmp(requested,"auto") ||
            !strcmp(requested,"avx2");
        if (allow && __builtin_cpu_supports("avx2")) cached=1;
        atomic_store_explicit(&backend,cached,memory_order_relaxed);
    }
    return cached;
}
#endif

void dsp_comb4_c(float *const line[4], const unsigned read[4],
    unsigned write, double input, double feedback, double sample_out[4]) {
    for (int k=0;k<4;k++) {
        double sample=line[k][read[k]];
        line[k][write]=(float)tanh(input+sample*feedback);
        sample_out[k]=sample;
    }
}

void dsp_comb4(float *const line[4], const unsigned read[4],
    unsigned write, double input, double feedback, double sample_out[4]) {
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2()) {
        dsp_comb4_avx2(line,read,write,input,feedback,sample_out);
        return;
    }
#endif
    dsp_comb4_c(line,read,write,input,feedback,sample_out);
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,no-fma"),noinline))
void dsp_comb4_avx2(float *const line[4], const unsigned read[4],
    unsigned write, double input, double feedback, double sample_out[4]) {
    double sample[4];
    for (int k=0;k<4;k++) sample[k]=line[k][read[k]];
    __m256d samples=_mm256_loadu_pd(sample);
    __m256d mix=_mm256_add_pd(_mm256_set1_pd(input),
        _mm256_mul_pd(samples,_mm256_set1_pd(feedback)));
    _mm256_storeu_pd(sample,mix);
    for (int k=0;k<4;k++) line[k][write]=(float)tanh(sample[k]);
    _mm256_storeu_pd(sample_out,samples);
}
#endif
