#include "vocoder_tails_simd.h"
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

__attribute__((noinline))
void vocoder_window_c(float *windowed, const float *input,
    const float *hann) {
    for (unsigned i=0;i<VOCODE_FFT;i++)
        windowed[i]=(float)((double)input[i]*hann[i]*1.62);
}

__attribute__((noinline))
void vocoder_magnitudes_c(float *magnitudes, const VocoderComplex *freq) {
    for (int i=0;i<=VOCODE_FFT/2;i++)
        magnitudes[i]=(float)(freq[i].re*freq[i].re+freq[i].im*freq[i].im);
}

__attribute__((noinline))
void vocoder_mirror_c(VocoderComplex *shifted) {
    for (int i=1;i<VOCODE_FFT/2;i++)
        shifted[VOCODE_FFT-i]=(VocoderComplex){shifted[i].re,-shifted[i].im};
}

__attribute__((noinline))
void vocoder_ola_c(float *sum, const VocoderComplex *shifted,
    const float *hann) {
    for (unsigned i=0;i<VOCODE_FFT;i++) {
        float sample=(float)shifted[i].re;
        sample=(float)((double)sample*hann[i]*1.62);
        sum[i]=(float)(sum[i]+sample/(VOCODE_FFT/VOCODE_HOP));
    }
}

#if defined(__x86_64__) || defined(__i386__)
/* Deinterleave four complexes held in two 256-bit loads into re/im quads:
   [r0,i0,r1,i1][r2,i2,r3,i3] -> re=[r0,r1,r2,r3], im=[i0,i1,i2,i3]. */
__attribute__((target("avx2,no-fma"),always_inline))
static inline void deinterleave4(__m256d a, __m256d b, __m256d *re,
    __m256d *im) {
    *re=_mm256_permute4x64_pd(_mm256_unpacklo_pd(a,b),0xD8);
    *im=_mm256_permute4x64_pd(_mm256_unpackhi_pd(a,b),0xD8);
}

__attribute__((target("avx2,no-fma"),noinline))
void vocoder_window_avx2(float *windowed, const float *input,
    const float *hann) {
    __m256d gain=_mm256_set1_pd(1.62);
    for (unsigned i=0;i<VOCODE_FFT;i+=4) {
        __m256d in=_mm256_cvtps_pd(_mm_loadu_ps(input+i));
        __m256d h=_mm256_cvtps_pd(_mm_loadu_ps(hann+i));
        __m256d p=_mm256_mul_pd(in,h);
        p=_mm256_mul_pd(p,gain);
        _mm_storeu_ps(windowed+i,_mm256_cvtpd_ps(p));
    }
}

__attribute__((target("avx2,no-fma"),noinline))
void vocoder_magnitudes_avx2(float *magnitudes, const VocoderComplex *freq) {
    const double *data=(const double *)freq;
    for (int i=0;i<VOCODE_FFT/2;i+=4) {
        __m256d a=_mm256_loadu_pd(data+2*i);
        __m256d b=_mm256_loadu_pd(data+2*i+4);
        __m256d re, im;
        deinterleave4(a,b,&re,&im);
        __m256d rr=_mm256_mul_pd(re,re);
        __m256d mm=_mm256_mul_pd(im,im);
        __m256d sum=_mm256_add_pd(rr,mm);
        _mm_storeu_ps(magnitudes+i,_mm256_cvtpd_ps(sum));
    }
    int last=VOCODE_FFT/2;
    magnitudes[last]=(float)(freq[last].re*freq[last].re+
        freq[last].im*freq[last].im);
}

__attribute__((target("avx2,no-fma"),noinline))
void vocoder_mirror_avx2(VocoderComplex *shifted) {
    /* Negate the imaginary lanes: [re,im] pairs become [re,-im]. */
    __m256d sign=_mm256_set_pd(-0.0,0.0,-0.0,0.0);
    int i;
    for (i=1;i+1<VOCODE_FFT/2;i+=2) {
        __m256d v=_mm256_loadu_pd((double *)&shifted[i]);
        v=_mm256_xor_pd(v,sign);
        /* Destinations 2048-i and 2047-i are adjacent; the pair swaps. */
        v=_mm256_permute4x64_pd(v,0x4E);
        _mm256_storeu_pd((double *)&shifted[VOCODE_FFT-1-i],v);
    }
    for (;i<VOCODE_FFT/2;i++)
        shifted[VOCODE_FFT-i]=(VocoderComplex){shifted[i].re,-shifted[i].im};
}

__attribute__((target("avx2,no-fma"),noinline))
void vocoder_ola_avx2(float *sum, const VocoderComplex *shifted,
    const float *hann) {
    const double *data=(const double *)shifted;
    __m256d gain=_mm256_set1_pd(1.62);
    __m128 sixth=_mm_set1_ps((float)(VOCODE_FFT/VOCODE_HOP));
    for (unsigned i=0;i<VOCODE_FFT;i+=4) {
        __m256d a=_mm256_loadu_pd(data+2*i);
        __m256d b=_mm256_loadu_pd(data+2*i+4);
        __m256d re, im;
        deinterleave4(a,b,&re,&im);
        (void)im;
        __m128 sample=_mm256_cvtpd_ps(re);
        __m256d wide=_mm256_cvtps_pd(sample);
        __m256d h=_mm256_cvtps_pd(_mm_loadu_ps(hann+i));
        __m256d p=_mm256_mul_pd(wide,h);
        p=_mm256_mul_pd(p,gain);
        __m128 scaled=_mm256_cvtpd_ps(p);
        __m128 add=_mm_div_ps(scaled,sixth);
        __m128 acc=_mm_loadu_ps(sum+i);
        acc=_mm_add_ps(acc,add);
        _mm_storeu_ps(sum+i,acc);
    }
}
#endif

__attribute__((noinline))
void vocoder_window(float *windowed, const float *input, const float *hann) {
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2()) {
        vocoder_window_avx2(windowed,input,hann);
        return;
    }
#endif
    vocoder_window_c(windowed,input,hann);
}

__attribute__((noinline))
void vocoder_magnitudes(float *magnitudes, const VocoderComplex *freq) {
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2()) {
        vocoder_magnitudes_avx2(magnitudes,freq);
        return;
    }
#endif
    vocoder_magnitudes_c(magnitudes,freq);
}

__attribute__((noinline))
void vocoder_mirror(VocoderComplex *shifted) {
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2()) {
        vocoder_mirror_avx2(shifted);
        return;
    }
#endif
    vocoder_mirror_c(shifted);
}

__attribute__((noinline))
void vocoder_ola(float *sum, const VocoderComplex *shifted,
    const float *hann) {
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2()) {
        vocoder_ola_avx2(sum,shifted,hann);
        return;
    }
#endif
    vocoder_ola_c(sum,shifted,hann);
}
