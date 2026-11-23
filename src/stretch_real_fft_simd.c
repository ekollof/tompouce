#include "stretch_real_fft_simd.h"
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

static void initial_c(double *out, const float *input,
                      const unsigned *real_reverse) {
    for (unsigned i=0;i<STRETCH_REAL_FFT_SIZE/2;i++) {
        unsigned index=real_reverse[i];
        double a=input[index], b=input[index+STRETCH_REAL_FFT_SIZE/2];
        out[4*i]=a+b;
        out[4*i+1]=0;
        out[4*i+2]=a-b;
        out[4*i+3]=0;
    }
}

static void stage_one(double *out, int base, int j, int q, int step,
                      const double *twiddle) {
    int a=base+j, b=a+q, c=b+q, d=c+q;
    int k=j*(step/2);
    double ma_re=out[2*a], ma_im=out[2*a+1];
    double mb_re=out[2*b]*twiddle[2*k]-out[2*b+1]*twiddle[2*k+1];
    double mb_im=out[2*b]*twiddle[2*k+1]+out[2*b+1]*twiddle[2*k];
    double mc_re=out[2*c]*twiddle[4*k]-out[2*c+1]*twiddle[4*k+1];
    double mc_im=out[2*c]*twiddle[4*k+1]+out[2*c+1]*twiddle[4*k];
    double md_re=out[2*d]*twiddle[6*k]-out[2*d+1]*twiddle[6*k+1];
    double md_im=out[2*d]*twiddle[6*k+1]+out[2*d+1]*twiddle[6*k];
    double t0_re=ma_re+mc_re, t0_im=ma_im+mc_im;
    double t1_re=ma_re-mc_re, t1_im=ma_im-mc_im;
    double t2_re=mb_re+md_re, t2_im=mb_im+md_im;
    double t3_re=mb_re-md_re, t3_im=mb_im-md_im;
    out[2*a]=t0_re+t2_re;
    out[2*a+1]=t0_im+t2_im;
    out[2*b]=t1_re+t3_im;
    out[2*b+1]=t1_im-t3_re;
    if (!j) {
        out[2*c]=t0_re-t2_re;
        out[2*c+1]=t0_im-t2_im;
        return;
    }
    if (j==q/2) return;
    out[2*(base+q-j)]=t1_re-t3_im;
    out[2*(base+q-j)+1]=-t1_im-t3_re;
    out[2*(base+2*q-j)]=t0_re-t2_re;
    out[2*(base+2*q-j)+1]=-t0_im+t2_im;
}

static void stages_c(double *out, const double *twiddle) {
    for (unsigned step=STRETCH_REAL_FFT_SIZE/4;step>=2;step/=4) {
        unsigned length=STRETCH_REAL_FFT_SIZE*2/step, q=length/4;
        for (unsigned base=0;base<STRETCH_REAL_FFT_SIZE;base+=length)
            for (unsigned j=0;j<=q/2;j++)
                stage_one(out,(int)base,(int)j,(int)q,(int)step,twiddle);
    }
}

__attribute__((noinline))
void stretch_real_fft_c(double *out, const float *input,
    const double *twiddle, const unsigned *real_reverse) {
    initial_c(out,input,real_reverse);
    stages_c(out,twiddle);
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,no-fma"),always_inline))
static inline __m256d complex_mul2(__m256d a, __m256d b) {
    __m256d real=_mm256_mul_pd(a,_mm256_movedup_pd(b));
    __m256d imaginary=_mm256_mul_pd(_mm256_permute_pd(a,0x5),
        _mm256_permute_pd(b,0xf));
    return _mm256_addsub_pd(real,imaginary);
}

__attribute__((target("avx2,no-fma"),always_inline))
static inline __m256d negate_imag(__m256d value) {
    return _mm256_xor_pd(value,_mm256_set_pd(-0.0,0.0,-0.0,0.0));
}

__attribute__((target("avx2,no-fma"),always_inline))
static inline __m256d swap_complex2(__m256d value) {
    return _mm256_permute4x64_pd(value,0x4e);
}

__attribute__((target("avx2,no-fma"),always_inline))
static inline void load_twiddles(const double *twiddle, int j, int step,
                                 double *w1, double *w2, double *w3) {
    for (int lane=0;lane<4;lane++) {
        int k=(j+lane)*(step/2);
        w1[2*lane]=twiddle[2*k];
        w1[2*lane+1]=twiddle[2*k+1];
        w2[2*lane]=twiddle[4*k];
        w2[2*lane+1]=twiddle[4*k+1];
        w3[2*lane]=twiddle[6*k];
        w3[2*lane+1]=twiddle[6*k+1];
    }
}

__attribute__((target("avx2,no-fma"),always_inline))
static inline void stage_group_avx2(double *out, int base, int j, int q,
                                    int step, const double *twiddle) {
    int a=base+j, b=a+q, c=b+q, d=c+q;
    __m256d ma0=_mm256_loadu_pd(out+2*a);
    __m256d ma1=_mm256_loadu_pd(out+2*a+4);
    __m256d mb0=_mm256_loadu_pd(out+2*b);
    __m256d mb1=_mm256_loadu_pd(out+2*b+4);
    __m256d mc0=_mm256_loadu_pd(out+2*c);
    __m256d mc1=_mm256_loadu_pd(out+2*c+4);
    __m256d md0=_mm256_loadu_pd(out+2*d);
    __m256d md1=_mm256_loadu_pd(out+2*d+4);
    double wb[8], wc[8], wd[8];
    load_twiddles(twiddle,j,step,wb,wc,wd);
    __m256d wb0=_mm256_loadu_pd(wb), wb1=_mm256_loadu_pd(wb+4);
    __m256d wc0=_mm256_loadu_pd(wc), wc1=_mm256_loadu_pd(wc+4);
    __m256d wd0=_mm256_loadu_pd(wd), wd1=_mm256_loadu_pd(wd+4);
    mb0=complex_mul2(mb0,wb0); mb1=complex_mul2(mb1,wb1);
    mc0=complex_mul2(mc0,wc0); mc1=complex_mul2(mc1,wc1);
    md0=complex_mul2(md0,wd0); md1=complex_mul2(md1,wd1);
    __m256d t0_0=_mm256_add_pd(ma0,mc0), t0_1=_mm256_add_pd(ma1,mc1);
    __m256d t1_0=_mm256_sub_pd(ma0,mc0), t1_1=_mm256_sub_pd(ma1,mc1);
    __m256d t2_0=_mm256_add_pd(mb0,md0), t2_1=_mm256_add_pd(mb1,md1);
    __m256d t3_0=_mm256_sub_pd(mb0,md0), t3_1=_mm256_sub_pd(mb1,md1);
    _mm256_storeu_pd(out+2*a,_mm256_add_pd(t0_0,t2_0));
    _mm256_storeu_pd(out+2*a+4,_mm256_add_pd(t0_1,t2_1));
    _mm256_storeu_pd(out+2*b,_mm256_add_pd(t1_0,
        negate_imag(_mm256_permute_pd(t3_0,0x5))));
    _mm256_storeu_pd(out+2*b+4,_mm256_add_pd(t1_1,
        negate_imag(_mm256_permute_pd(t3_1,0x5))));
    __m256d m10=_mm256_sub_pd(negate_imag(t1_0),
        _mm256_permute_pd(t3_0,0x5));
    __m256d m11=_mm256_sub_pd(negate_imag(t1_1),
        _mm256_permute_pd(t3_1,0x5));
    __m256d m20=negate_imag(_mm256_sub_pd(t0_0,t2_0));
    __m256d m21=negate_imag(_mm256_sub_pd(t0_1,t2_1));
    int mirror1=base+q-j-3, mirror2=base+2*q-j-3;
    _mm256_storeu_pd(out+2*mirror1,swap_complex2(m11));
    _mm256_storeu_pd(out+2*mirror1+4,swap_complex2(m10));
    _mm256_storeu_pd(out+2*mirror2,swap_complex2(m21));
    _mm256_storeu_pd(out+2*mirror2+4,swap_complex2(m20));
}
#endif

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,no-fma"),noinline))
static void stage_avx2_all(double *out, unsigned step,
                           const double *twiddle) {
    unsigned length=STRETCH_REAL_FFT_SIZE*2/step, q=length/4;
    for (unsigned base=0;base<STRETCH_REAL_FFT_SIZE;base+=length) {
        if (q<32) {
            for (unsigned j=0;j<=q/2;j++)
                stage_one(out,(int)base,(int)j,(int)q,(int)step,twiddle);
            continue;
        }
        stage_one(out,(int)base,0,(int)q,(int)step,twiddle);
        unsigned j=1;
        for (;j+3<=q/2-1;j+=4)
            stage_group_avx2(out,(int)base,(int)j,(int)q,(int)step,twiddle);
        for (;j<q/2;j++)
            stage_one(out,(int)base,(int)j,(int)q,(int)step,twiddle);
        stage_one(out,(int)base,(int)(q/2),(int)q,(int)step,twiddle);
    }
}

__attribute__((target("avx2,no-fma"),noinline))
void stretch_real_fft_avx2(double *out, const float *input,
    const double *twiddle, const unsigned *real_reverse) {
    initial_c(out,input,real_reverse);
    for (unsigned step=STRETCH_REAL_FFT_SIZE/4;step>=2;step/=4)
        stage_avx2_all(out,step,twiddle);
}
#endif

void stretch_real_fft(double *out, const float *input,
    const double *twiddle, const unsigned *real_reverse) {
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2()) {
        stretch_real_fft_avx2(out,input,twiddle,real_reverse);
        return;
    }
#endif
    stretch_real_fft_c(out,input,twiddle,real_reverse);
}
