#include "dsp_supersaw.h"
#include "convolution_mix.h"
#include "common.h"
#include <float.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#if DSP_SUPERSAW_HAVE_AVX2
#include <immintrin.h>
#endif

/* The unison bank has no cross-oscillator state. Each lane performs exactly the
   scalar operations below; the caller still sums in unison-index order. */
static double blep(double phase, double step) {
    if (step <= 0) return 0;
    if (phase < step) { double t = phase/step; return t+t-t*t-1; }
    if (phase > 1-step) { double t = (phase-1)/step; return t*t+t+t+1; }
    return 0;
}

static void values_c_range(DspState *s, double frequency, double *values,
                           unsigned begin, unsigned end) {
    for (unsigned i=begin;i<end;i++) {
        double rate=frequency*s->saw_ratio[i]/SR, x=0;
        /* Near-one fractional unison and extreme spreads can overflow the
           exponential. Keep invalid oscillators silent and state finite. */
        if (s->saw_ratio[i]>0 && isfinite(rate)) {
            rate-=floor(rate);
            double phase=s->saw_phase[i];
            x=2*phase-1-blep(phase,fmin(rate,1-rate));
            phase+=rate;
            if (phase>=1) phase-=1;
            s->saw_phase[i]=phase;
        }
        values[i]=x;
    }
}

void dsp_supersaw_values_c(DspState *s, double frequency, double *values) {
    values_c_range(s,frequency,values,0,s->supersaw_voices);
}

#if DSP_SUPERSAW_HAVE_AVX2
__attribute__((target("avx2,no-fma"),always_inline))
static inline __m256d blep4(__m256d phase, __m256d step) {
    const __m256d one=_mm256_set1_pd(1.0), zero=_mm256_setzero_pd();
    __m256d t1=_mm256_div_pd(phase,step);
    __m256d r1=_mm256_sub_pd(_mm256_sub_pd(_mm256_add_pd(t1,t1),
        _mm256_mul_pd(t1,t1)),one);
    __m256d t2=_mm256_div_pd(_mm256_sub_pd(phase,one),step);
    __m256d r2=_mm256_mul_pd(t2,t2);
    r2=_mm256_add_pd(r2,t2);
    r2=_mm256_add_pd(r2,t2);
    r2=_mm256_add_pd(r2,one);
    __m256d positive=_mm256_cmp_pd(step,zero,_CMP_GT_OQ);
    __m256d first=_mm256_and_pd(positive,
        _mm256_cmp_pd(phase,step,_CMP_LT_OQ));
    __m256d second=_mm256_and_pd(positive,
        _mm256_cmp_pd(phase,_mm256_sub_pd(one,step),_CMP_GT_OQ));
    __m256d result=_mm256_setzero_pd();
    result=_mm256_blendv_pd(result,r1,first);
    result=_mm256_blendv_pd(result,r2,second);
    return result;
}

__attribute__((target("avx2,no-fma"),noinline))
void dsp_supersaw_values_avx2(DspState *s, double frequency, double *values) {
    const __m256d one=_mm256_set1_pd(1.0);
    const __m256d zero=_mm256_setzero_pd();
    const __m256d maximum=_mm256_set1_pd(DBL_MAX);
    __m256d freq=_mm256_set1_pd(frequency);
    unsigned i=0, count=s->supersaw_voices;
    for (;i+4<=count;i+=4) {
        __m256d ratio=_mm256_loadu_pd(s->saw_ratio+i);
        __m256d phase=_mm256_loadu_pd(s->saw_phase+i);
        __m256d rate=_mm256_div_pd(_mm256_mul_pd(freq,ratio),
            _mm256_set1_pd(SR));
        /* ratio>0 and isfinite(rate); ordered comparisons reject NaN. */
        __m256d valid=_mm256_and_pd(
            _mm256_cmp_pd(ratio,zero,_CMP_GT_OQ),
            _mm256_and_pd(
                _mm256_cmp_pd(rate,maximum,_CMP_LE_OQ),
                _mm256_cmp_pd(rate,_mm256_sub_pd(zero,maximum),_CMP_GE_OQ)));
        __m256d fraction=_mm256_sub_pd(rate,_mm256_floor_pd(rate));
        __m256d step=_mm256_min_pd(fraction,_mm256_sub_pd(one,fraction));
        __m256d x=_mm256_sub_pd(_mm256_sub_pd(_mm256_add_pd(phase,phase),one),
            blep4(phase,step));
        x=_mm256_and_pd(x,valid);
        __m256d next=_mm256_add_pd(phase,fraction);
        __m256d wrap=_mm256_cmp_pd(next,one,_CMP_GE_OQ);
        next=_mm256_sub_pd(next,_mm256_and_pd(wrap,one));
        next=_mm256_blendv_pd(phase,next,valid);
        _mm256_storeu_pd(values+i,x);
        _mm256_storeu_pd(s->saw_phase+i,next);
    }
    if (i<count) values_c_range(s,frequency,values,i,count);
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

void dsp_supersaw_values(DspState *s, double frequency, double *values) {
#if DSP_SUPERSAW_HAVE_AVX2
    if (use_avx2()) { dsp_supersaw_values_avx2(s,frequency,values); return; }
#endif
    dsp_supersaw_values_c(s,frequency,values);
}
