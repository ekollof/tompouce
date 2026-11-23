#include "oscillator_simd.h"
#include "common.h"
#include "dsp.h"
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

void dsp_saw4(const double phase[4], const double freq[4], double output[4]) {
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2()) {
        dsp_saw4_avx2(phase,freq,output);
        return;
    }
#endif
    dsp_saw4_c(phase,freq,output);
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,no-fma"),always_inline))
static inline __m256d poly_blep4(__m256d phase, __m256d step) {
    const __m256d one=_mm256_set1_pd(1.0);
    const __m256d zero=_mm256_setzero_pd();
    __m256d first=_mm256_cmp_pd(phase,step,_CMP_LT_OQ);
    __m256d second=_mm256_cmp_pd(phase,_mm256_sub_pd(one,step),
        _CMP_GT_OQ);
    __m256d u1=_mm256_div_pd(phase,step);
    __m256d b1=_mm256_sub_pd(_mm256_sub_pd(_mm256_add_pd(u1,u1),
        _mm256_mul_pd(u1,u1)),one);
    __m256d u2=_mm256_div_pd(_mm256_sub_pd(phase,one),step);
    __m256d b2=_mm256_add_pd(_mm256_add_pd(_mm256_add_pd(
        _mm256_mul_pd(u2,u2),u2),u2),one);
    __m256d blep=_mm256_blendv_pd(zero,b2,second);
    blep=_mm256_blendv_pd(blep,b1,first);
    return _mm256_and_pd(blep,_mm256_cmp_pd(step,zero,_CMP_GT_OQ));
}
#endif

void dsp_saw4_c(const double phase[4], const double freq[4], double output[4]) {
    for (int i=0;i<4;i++)
        output[i]=dsp_oscillator(0,phase[i],freq[i],.5);
}

void dsp_square4_c(const double phase[4], const double freq[4],
    const double width[4], double output[4]) {
    for (int i=0;i<4;i++)
        output[i]=dsp_oscillator(1,phase[i],freq[i],width[i]);
}

void dsp_square4(const double phase[4], const double freq[4],
    const double width[4], double output[4]) {
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2()) {
        dsp_square4_avx2(phase,freq,width,output);
        return;
    }
#endif
    dsp_square4_c(phase,freq,width,output);
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,no-fma"),noinline))
void dsp_saw4_avx2(const double phase[4], const double freq[4],
    double output[4]) {
    const __m256d half=_mm256_set1_pd(.5);
    const __m256d one=_mm256_set1_pd(1.0);
    const __m256d two=_mm256_set1_pd(2.0);
    const __m256d maximum=_mm256_set1_pd(.49);
    const __m256d zero=_mm256_setzero_pd();
    __m256d ph=_mm256_loadu_pd(phase);
    __m256d frequency=_mm256_loadu_pd(freq);
    __m256d t=_mm256_div_pd(ph,_mm256_set1_pd(2*M_PI));
    t=_mm256_sub_pd(t,_mm256_floor_pd(t));
    __m256d step=_mm256_min_pd(maximum,
        _mm256_div_pd(_mm256_max_pd(frequency,_mm256_sub_pd(zero,frequency)),
            _mm256_set1_pd(SR)));
    __m256d shifted=_mm256_add_pd(t,half);
    shifted=_mm256_sub_pd(shifted,_mm256_floor_pd(shifted));
    __m256d first=_mm256_cmp_pd(shifted,step,_CMP_LT_OQ);
    __m256d second=_mm256_cmp_pd(shifted,_mm256_sub_pd(one,step),
        _CMP_GT_OQ);
    __m256d u1=_mm256_div_pd(shifted,step);
    __m256d b1=_mm256_sub_pd(_mm256_sub_pd(_mm256_add_pd(u1,u1),
        _mm256_mul_pd(u1,u1)),one);
    __m256d u2=_mm256_div_pd(_mm256_sub_pd(shifted,one),step);
    __m256d b2=_mm256_add_pd(_mm256_add_pd(_mm256_add_pd(
        _mm256_mul_pd(u2,u2),u2),u2),one);
    __m256d blep=_mm256_blendv_pd(zero,b2,second);
    blep=_mm256_blendv_pd(blep,b1,first);
    blep=_mm256_and_pd(blep,
        _mm256_cmp_pd(step,zero,_CMP_GT_OQ));
    __m256d result=_mm256_sub_pd(_mm256_sub_pd(
        _mm256_mul_pd(two,shifted),one),blep);
    _mm256_storeu_pd(output,result);
}

__attribute__((target("avx2,no-fma"),noinline))
void dsp_square4_avx2(const double phase[4], const double freq[4],
    const double width[4], double output[4]) {
    const __m256d one=_mm256_set1_pd(1.0);
    const __m256d minimum=_mm256_set1_pd(.01);
    const __m256d maximum=_mm256_set1_pd(.99);
    const __m256d zero=_mm256_setzero_pd();
    __m256d ph=_mm256_loadu_pd(phase);
    __m256d frequency=_mm256_loadu_pd(freq);
    __m256d w=_mm256_loadu_pd(width);
    __m256d t=_mm256_div_pd(ph,_mm256_set1_pd(2*M_PI));
    t=_mm256_sub_pd(t,_mm256_floor_pd(t));
    __m256d step=_mm256_min_pd(_mm256_set1_pd(.49),
        _mm256_div_pd(_mm256_max_pd(frequency,
            _mm256_sub_pd(zero,frequency)),_mm256_set1_pd(SR)));
    w=_mm256_blendv_pd(w,minimum,_mm256_cmp_pd(w,minimum,_CMP_LT_OQ));
    w=_mm256_blendv_pd(w,maximum,_mm256_cmp_pd(w,maximum,_CMP_GT_OQ));
    __m256d edge=_mm256_sub_pd(t,w);
    edge=_mm256_sub_pd(edge,_mm256_floor_pd(edge));
    __m256d sign=_mm256_blendv_pd(_mm256_sub_pd(zero,one),one,
        _mm256_cmp_pd(t,w,_CMP_LT_OQ));
    __m256d rising=poly_blep4(t,step);
    __m256d falling=poly_blep4(edge,step);
    __m256d result=_mm256_sub_pd(_mm256_add_pd(sign,rising),falling);
    _mm256_storeu_pd(output,result);
}
#endif
