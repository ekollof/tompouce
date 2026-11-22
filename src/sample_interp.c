#include "sample_interp.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

void sample_interp2_c(const float *samples, int frames, double position,
                      double *left, double *right) {
    if (!samples || frames <= 0 || !isfinite(position) || position < 0 ||
        position >= frames) {
        if (left) *left=0;
        if (right) *right=0;
        return;
    }
    int index=(int)position;
    double fraction=position-index;
    double a_l=samples[(size_t)index*2];
    double a_r=samples[(size_t)index*2+1];
    double b_l=a_l, b_r=a_r;
    if (index+1<frames) {
        b_l=samples[(size_t)(index+1)*2];
        b_r=samples[(size_t)(index+1)*2+1];
    }
    if (left) *left=a_l+(b_l-a_l)*fraction;
    if (right) *right=a_r+(b_r-a_r)*fraction;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,no-fma"),noinline))
void sample_interp2_avx2(const float *samples, int frames, double position,
                         double *left, double *right) {
    if (!samples || frames <= 0 || !isfinite(position) || position < 0 ||
        position >= frames) {
        if (left) *left=0;
        if (right) *right=0;
        return;
    }
    int index=(int)position;
    double fraction=position-index;
    __m128 a_f=_mm_castsi128_ps(_mm_loadl_epi64(
        (const __m128i*)&samples[(size_t)index*2]));
    __m128 b_f=a_f;
    if (index+1<frames)
        b_f=_mm_castsi128_ps(_mm_loadl_epi64(
            (const __m128i*)&samples[(size_t)(index+1)*2]));
    __m128d a_lo=_mm_cvtps_pd(a_f), b_lo=_mm_cvtps_pd(b_f);
    /* [current,next] and [next,current], then the same per-channel
       expression for the lower pair. */
    __m256d both=_mm256_set_m128d(b_lo,a_lo);
    __m256d next=_mm256_set_m128d(a_lo,b_lo);
    __m256d result=_mm256_add_pd(both,_mm256_mul_pd(
        _mm256_sub_pd(next,both),_mm256_set1_pd(fraction)));
    __m128d pair=_mm256_castpd256_pd128(result);
    if (left) _mm_storel_pd(left,pair);
    if (right) _mm_storeh_pd(right,pair);
}
#endif

static atomic_int backend=-1; /* -1 undecided, 0 C, 1 AVX2 */

static int use_avx2(void) {
    int cached=atomic_load_explicit(&backend,memory_order_relaxed);
    if (cached<0) {
        cached=0;
        const char *requested=getenv("TOMPOUCE_SIMD");
#if defined(__x86_64__) || defined(__i386__)
        __builtin_cpu_init();
        int allow=!requested || !*requested || !strcmp(requested,"auto") ||
            !strcmp(requested,"avx2");
        if (allow && __builtin_cpu_supports("avx2")) cached=1;
#endif
        atomic_store_explicit(&backend,cached,memory_order_relaxed);
    }
    return cached;
}

void sample_interp2(const float *samples, int frames, double position,
                    double *left, double *right) {
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2()) {
        sample_interp2_avx2(samples,frames,position,left,right);
        return;
    }
#endif
    sample_interp2_c(samples,frames,position,left,right);
}
