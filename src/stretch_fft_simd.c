#include "stretch_fft_simd.h"
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

static void stage_c(double *data, unsigned length, int inverse,
                    const double *twiddle) {
    unsigned half=length/2, stride=STRETCH_FFT_SIZE/length;
    for (unsigned base=0;base<STRETCH_FFT_SIZE;base+=length)
        for (unsigned j=0;j<half;j++) {
            unsigned at=base+j;
            double *a=data+2*at, *b=a+2*half;
            const double *w=twiddle+2*(j*stride);
            double ar=a[0], ai=a[1], br=b[0], bi=b[1];
            double wr=w[0], wi=inverse?-w[1]:w[1];
            double re=br*wr-bi*wi;
            double im=br*wi+bi*wr;
            data[2*at]=ar+re;
            data[2*at+1]=ai+im;
            data[2*(at+half)]=ar-re;
            data[2*(at+half)+1]=ai-im;
        }
}

static void fft_c(double *data, int inverse, const double *twiddle,
                  const unsigned *reverse_bits) {
    for (unsigned i=0;i<STRETCH_FFT_SIZE;i++)
        if (i<reverse_bits[i]) {
            unsigned j=reverse_bits[i];
            double re=data[2*i], im=data[2*i+1];
            data[2*i]=data[2*j];
            data[2*i+1]=data[2*j+1];
            data[2*j]=re;
            data[2*j+1]=im;
        }
    for (unsigned length=2;length<=STRETCH_FFT_SIZE;length*=2)
        stage_c(data,length,inverse,twiddle);
    if (inverse)
        for (unsigned i=0;i<STRETCH_FFT_SIZE;i++) {
            data[2*i]/=STRETCH_FFT_SIZE;
            data[2*i+1]/=STRETCH_FFT_SIZE;
        }
}

__attribute__((noinline))
void stretch_fft_c(double *data, int inverse, const double *twiddle,
    const unsigned *reverse_bits) {
    fft_c(data,inverse,twiddle,reverse_bits);
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,no-fma"),always_inline))
static inline __m256d complex_mul2(__m256d a, __m256d b) {
    /* [ar,ai,ar1,ai1] * [br,bi,br1,bi1] with two interleaved complex lanes. */
    __m256d real=_mm256_mul_pd(a,_mm256_movedup_pd(b));
    __m256d imaginary=_mm256_mul_pd(_mm256_permute_pd(a,0x5),
        _mm256_permute_pd(b,0xf));
    return _mm256_addsub_pd(real,imaginary);
}

__attribute__((target("avx2,no-fma"),noinline))
static void stage_avx2(double *data, unsigned length, int inverse,
                       const double *twiddle) {
    unsigned half=length/2, stride=STRETCH_FFT_SIZE/length;
    for (unsigned base=0;base<STRETCH_FFT_SIZE;base+=length)
        for (unsigned j=0;j<half;j+=4) {
            unsigned at=base+j;
            double *pa=data+2*at, *pb=pa+2*half;
            __m256d a0=_mm256_loadu_pd(pa), a1=_mm256_loadu_pd(pa+4);
            __m256d b0=_mm256_loadu_pd(pb), b1=_mm256_loadu_pd(pb+4);
            double w[8];
            for (int lane=0;lane<4;lane++) {
                const double *value=twiddle+2*((j+(unsigned)lane)*stride);
                w[2*lane]=value[0];
                w[2*lane+1]=inverse?-value[1]:value[1];
            }
            __m256d w0=_mm256_loadu_pd(w), w1=_mm256_loadu_pd(w+4);
            __m256d product0=complex_mul2(b0,w0);
            __m256d product1=complex_mul2(b1,w1);
            _mm256_storeu_pd(pa,_mm256_add_pd(a0,product0));
            _mm256_storeu_pd(pa+4,_mm256_add_pd(a1,product1));
            _mm256_storeu_pd(pb,_mm256_sub_pd(a0,product0));
            _mm256_storeu_pd(pb+4,_mm256_sub_pd(a1,product1));
        }
}

__attribute__((target("avx2,no-fma"),noinline))
void stretch_fft_avx2(double *data, int inverse, const double *twiddle,
    const unsigned *reverse_bits) {
    for (unsigned i=0;i<STRETCH_FFT_SIZE;i++)
        if (i<reverse_bits[i]) {
            unsigned j=reverse_bits[i];
            double re=data[2*i], im=data[2*i+1];
            data[2*i]=data[2*j];
            data[2*i+1]=data[2*j+1];
            data[2*j]=re;
            data[2*j+1]=im;
        }
    for (unsigned length=2;length<=STRETCH_FFT_SIZE;length*=2) {
        if (length<8) stage_c(data,length,inverse,twiddle);
        else stage_avx2(data,length,inverse,twiddle);
    }
    if (inverse)
        for (unsigned i=0;i<STRETCH_FFT_SIZE;i++) {
            data[2*i]/=STRETCH_FFT_SIZE;
            data[2*i+1]/=STRETCH_FFT_SIZE;
        }
}
#endif

void stretch_fft(double *data, int inverse, const double *twiddle,
    const unsigned *reverse_bits) {
#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2()) {
        stretch_fft_avx2(data,inverse,twiddle,reverse_bits);
        return;
    }
#endif
    stretch_fft_c(data,inverse,twiddle,reverse_bits);
}
