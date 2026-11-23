#include "visual_fft.h"
#include <math.h>
#include <pthread.h>
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

#define VISUAL_FFT_MAX 4096
#define VISUAL_TAU 6.2831853071795864769
static double twiddle_re[VISUAL_FFT_MAX], twiddle_im[VISUAL_FFT_MAX];
static pthread_once_t tables_once=PTHREAD_ONCE_INIT;

static void init_tables(void) {
    for (int length=2;length<=VISUAL_FFT_MAX;length*=2) {
        int offset=length/2-1;
        for (int j=0;j<length/2;j++) {
            double angle=-VISUAL_TAU*j/length;
            twiddle_re[offset+j]=cos(angle);
            twiddle_im[offset+j]=sin(angle);
        }
    }
}

static void tables(void) { pthread_once(&tables_once,init_tables); }

static void reverse_bits(double *re, int n) {
    for (int i=1,j=0;i<n;i++) {
        int bit=n>>1;
        for (;j&bit;bit>>=1) j^=bit;
        j^=bit;
        if (i<j) { double value=re[i]; re[i]=re[j]; re[j]=value; }
    }
}

static void butterflies_c(double *re, double *imag, int n, int length) {
    int half=length/2, offset=half-1;
    for (int start=0;start<n;start+=length)
        for (int j=0;j<half;j++) {
            double c=twiddle_re[offset+j], s=twiddle_im[offset+j];
            int u=start+j, v=u+half;
            double r=c*re[v]-s*imag[v];
            double q=s*re[v]+c*imag[v];
            re[v]=re[u]-r;
            imag[v]=imag[u]-q;
            re[u]+=r;
            imag[u]+=q;
        }
}

__attribute__((noinline))
void visual_fft_c(double *re, double *imag, int n) {
    tables();
    reverse_bits(re,n);
    for (int length=2;length<=n;length*=2)
        butterflies_c(re,imag,n,length);
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,no-fma"),noinline))
void visual_fft_avx2(double *re, double *imag, int n) {
    tables();
    reverse_bits(re,n);
    for (int length=2;length<=n;length*=2) {
        if (length<8) {
            butterflies_c(re,imag,n,length);
            continue;
        }
        int half=length/2, offset=half-1;
        for (int start=0;start<n;start+=length)
            for (int j=0;j<half;j+=4) {
                int u=start+j, v=u+half;
                __m256d re_u=_mm256_loadu_pd(re+u);
                __m256d im_u=_mm256_loadu_pd(imag+u);
                __m256d re_v=_mm256_loadu_pd(re+v);
                __m256d im_v=_mm256_loadu_pd(imag+v);
                __m256d c=_mm256_loadu_pd(twiddle_re+offset+j);
                __m256d s=_mm256_loadu_pd(twiddle_im+offset+j);
                __m256d r=_mm256_sub_pd(_mm256_mul_pd(c,re_v),
                    _mm256_mul_pd(s,im_v));
                __m256d q=_mm256_add_pd(_mm256_mul_pd(s,re_v),
                    _mm256_mul_pd(c,im_v));
                _mm256_storeu_pd(re+v,_mm256_sub_pd(re_u,r));
                _mm256_storeu_pd(imag+v,_mm256_sub_pd(im_u,q));
                _mm256_storeu_pd(re+u,_mm256_add_pd(re_u,r));
                _mm256_storeu_pd(imag+u,_mm256_add_pd(im_u,q));
            }
    }
}
#endif

void visual_fft(double *re, double *imag, int n) {
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2()) {
        visual_fft_avx2(re,imag,n);
        return;
    }
#endif
    visual_fft_c(re,imag,n);
}
