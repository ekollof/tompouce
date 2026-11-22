#include "voice_soa_dsp.h"
#include "convolution_mix.h"
#include <float.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#if VOICE_SOA_DSP_HAVE_AVX2
#include <immintrin.h>
#endif

void dsp_biquad2_init(DspBiquad2 *dst, const DspBiquad source[2]) {
    for (unsigned lane=0;lane<2;lane++) {
        dst->b0[lane]=source[lane].b0; dst->b1[lane]=source[lane].b1;
        dst->b2[lane]=source[lane].b2; dst->a1[lane]=source[lane].a1;
        dst->a2[lane]=source[lane].a2; dst->z1[lane]=source[lane].z1;
        dst->z2[lane]=source[lane].z2;
    }
}

void dsp_biquad2_process_c(DspBiquad2 *state, const double *input,
                           double *output, unsigned frames) {
    for (unsigned frame=0;frame<frames;frame++) {
        for (unsigned lane=0;lane<2;lane++) {
            double x=input[frame*2+lane];
            double y=state->b0[lane]*x+state->z1[lane];
            state->z1[lane]=state->b1[lane]*x-state->a1[lane]*y+state->z2[lane];
            state->z2[lane]=state->b2[lane]*x-state->a2[lane]*y;
            if (!isfinite(y) || !isfinite(state->z1[lane]) ||
                !isfinite(state->z2[lane])) {
                state->z1[lane]=state->z2[lane]=0;
                y=0;
            }
            output[frame*2+lane]=y;
        }
    }
}

#if VOICE_SOA_DSP_HAVE_AVX2
__attribute__((target("avx2,no-fma"),noinline))
void dsp_biquad2_process_avx2(DspBiquad2 *state, const double *input,
                              double *output, unsigned frames) {
    /* Two channels do not fill a 256-bit lane; duplicate the pair so the
       kernel still exercises the AVX2 path while storing only the real pair. */
    __m256d b0=_mm256_set_m128d(_mm_loadu_pd(state->b0),_mm_loadu_pd(state->b0));
    __m256d b1=_mm256_set_m128d(_mm_loadu_pd(state->b1),_mm_loadu_pd(state->b1));
    __m256d b2=_mm256_set_m128d(_mm_loadu_pd(state->b2),_mm_loadu_pd(state->b2));
    __m256d a1=_mm256_set_m128d(_mm_loadu_pd(state->a1),_mm_loadu_pd(state->a1));
    __m256d a2=_mm256_set_m128d(_mm_loadu_pd(state->a2),_mm_loadu_pd(state->a2));
    __m256d z1=_mm256_set_m128d(_mm_loadu_pd(state->z1),_mm_loadu_pd(state->z1));
    __m256d z2=_mm256_set_m128d(_mm_loadu_pd(state->z2),_mm_loadu_pd(state->z2));
    const __m256d maximum=_mm256_set1_pd(DBL_MAX);
    const __m256d minimum=_mm256_set1_pd(-DBL_MAX);
    for (unsigned frame=0;frame<frames;frame++) {
        __m128d x=_mm_loadu_pd(input+frame*2);
        __m256d vx=_mm256_set_m128d(x,x);
        __m256d y=_mm256_add_pd(_mm256_mul_pd(b0,vx),z1);
        z1=_mm256_add_pd(_mm256_sub_pd(_mm256_mul_pd(b1,vx),
            _mm256_mul_pd(a1,y)),z2);
        z2=_mm256_sub_pd(_mm256_mul_pd(b2,vx),_mm256_mul_pd(a2,y));
        __m256d finite=_mm256_and_pd(
            _mm256_cmp_pd(y,minimum,_CMP_GE_OQ),
            _mm256_cmp_pd(y,maximum,_CMP_LE_OQ));
        finite=_mm256_and_pd(finite,_mm256_and_pd(
            _mm256_cmp_pd(z1,minimum,_CMP_GE_OQ),
            _mm256_cmp_pd(z1,maximum,_CMP_LE_OQ)));
        finite=_mm256_and_pd(finite,_mm256_and_pd(
            _mm256_cmp_pd(z2,minimum,_CMP_GE_OQ),
            _mm256_cmp_pd(z2,maximum,_CMP_LE_OQ)));
        y=_mm256_and_pd(y,finite);
        z1=_mm256_and_pd(z1,finite);
        z2=_mm256_and_pd(z2,finite);
        _mm_storeu_pd(output+frame*2,_mm256_castpd256_pd128(y));
    }
    _mm_storeu_pd(state->z1,_mm256_castpd256_pd128(z1));
    _mm_storeu_pd(state->z2,_mm256_castpd256_pd128(z2));
}
#endif

void dsp_biquad4_init(DspBiquad4 *dst, const DspBiquad source[4]) {
    for (unsigned lane=0;lane<4;lane++) {
        dst->b0[lane]=source[lane].b0; dst->b1[lane]=source[lane].b1;
        dst->b2[lane]=source[lane].b2; dst->a1[lane]=source[lane].a1;
        dst->a2[lane]=source[lane].a2; dst->z1[lane]=source[lane].z1;
        dst->z2[lane]=source[lane].z2;
    }
}

void dsp_biquad4_process_c(DspBiquad4 *state, const double *input,
                           double *output, unsigned frames) {
    for (unsigned frame=0;frame<frames;frame++) {
        for (unsigned lane=0;lane<4;lane++) {
            double x=input[frame*4+lane];
            double y=state->b0[lane]*x+state->z1[lane];
            state->z1[lane]=state->b1[lane]*x-state->a1[lane]*y+state->z2[lane];
            state->z2[lane]=state->b2[lane]*x-state->a2[lane]*y;
            if (!isfinite(y) || !isfinite(state->z1[lane]) ||
                !isfinite(state->z2[lane])) {
                state->z1[lane]=state->z2[lane]=0;
                y=0;
            }
            output[frame*4+lane]=y;
        }
    }
}

#if VOICE_SOA_DSP_HAVE_AVX2
__attribute__((target("avx2,no-fma"),noinline))
void dsp_biquad4_process_avx2(DspBiquad4 *state, const double *input,
                              double *output, unsigned frames) {
    __m256d b0=_mm256_loadu_pd(state->b0), b1=_mm256_loadu_pd(state->b1);
    __m256d b2=_mm256_loadu_pd(state->b2), a1=_mm256_loadu_pd(state->a1);
    __m256d a2=_mm256_loadu_pd(state->a2);
    __m256d z1=_mm256_loadu_pd(state->z1), z2=_mm256_loadu_pd(state->z2);
    const __m256d maximum=_mm256_set1_pd(DBL_MAX);
    const __m256d minimum=_mm256_set1_pd(-DBL_MAX);
    for (unsigned frame=0;frame<frames;frame++) {
        __m256d x=_mm256_loadu_pd(input+frame*4);
        __m256d y=_mm256_add_pd(_mm256_mul_pd(b0,x),z1);
        z1=_mm256_add_pd(_mm256_sub_pd(_mm256_mul_pd(b1,x),
            _mm256_mul_pd(a1,y)),z2);
        z2=_mm256_sub_pd(_mm256_mul_pd(b2,x),_mm256_mul_pd(a2,y));
        __m256d finite=_mm256_and_pd(
            _mm256_cmp_pd(y,minimum,_CMP_GE_OQ),
            _mm256_cmp_pd(y,maximum,_CMP_LE_OQ));
        finite=_mm256_and_pd(finite,_mm256_and_pd(
            _mm256_cmp_pd(z1,minimum,_CMP_GE_OQ),
            _mm256_cmp_pd(z1,maximum,_CMP_LE_OQ)));
        finite=_mm256_and_pd(finite,_mm256_and_pd(
            _mm256_cmp_pd(z2,minimum,_CMP_GE_OQ),
            _mm256_cmp_pd(z2,maximum,_CMP_LE_OQ)));
        y=_mm256_and_pd(y,finite);
        z1=_mm256_and_pd(z1,finite);
        z2=_mm256_and_pd(z2,finite);
        _mm256_storeu_pd(output+frame*4,y);
    }
    _mm256_storeu_pd(state->z1,z1); _mm256_storeu_pd(state->z2,z2);
}
#endif

static atomic_int avx2_choice=-1;
static int use_avx2(void) {
    int cached=atomic_load(&avx2_choice);
    if (cached<0) {
        convolution_simd_init();
        cached=strcmp(convolution_simd_name(),"avx2")==0;
        atomic_store(&avx2_choice,cached);
    }
    return cached;
}

void dsp_biquad2_process(DspBiquad2 *state, const double *input,
                         double *output, unsigned frames) {
#if VOICE_SOA_DSP_HAVE_AVX2
    if (use_avx2()) {
        dsp_biquad2_process_avx2(state,input,output,frames);
        return;
    }
#endif
    dsp_biquad2_process_c(state,input,output,frames);
}

void dsp_biquad4_process(DspBiquad4 *state, const double *input,
                         double *output, unsigned frames) {
#if VOICE_SOA_DSP_HAVE_AVX2
    if (use_avx2()) {
        dsp_biquad4_process_avx2(state,input,output,frames);
        return;
    }
#endif
    dsp_biquad4_process_c(state,input,output,frames);
}
