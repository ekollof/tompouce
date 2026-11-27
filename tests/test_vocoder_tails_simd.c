/* Exactness of the vocoder tail kernels against their C references. */
#include "../src/vocoder_tails_simd.c"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static uint32_t random_state=0x51edB00bu;
static double random_value(double scale) {
    random_state=random_state*1664525u+1013904223u;
    return (double)(random_state>>8)/16777216.0*scale-scale/2;
}

static void fill_float(float *data, unsigned n, double scale) {
    for (unsigned i=0;i<n;i++) data[i]=(float)random_value(scale);
}

static void fill_complex(VocoderComplex *data, unsigned n, double scale) {
    for (unsigned i=0;i<n;i++) {
        data[i].re=random_value(scale);
        data[i].im=random_value(scale);
    }
}

static void compare_window(void) {
    static float input[VOCODE_FFT], hann[VOCODE_FFT];
    static float a[VOCODE_FFT], b[VOCODE_FFT], c[VOCODE_FFT];
    fill_float(input,VOCODE_FFT,2.0);
    fill_float(hann,VOCODE_FFT,2.0);
    hann[0]=0; hann[VOCODE_FFT-1]=1.5f;
    vocoder_window_c(a,input,hann);
#if VOCODE_TAILS_HAVE_AVX2
    vocoder_window_avx2(b,input,hann);
    assert(memcmp(a,b,sizeof(a))==0);
#endif
    vocoder_window(c,input,hann);
    assert(memcmp(a,c,sizeof(a))==0);
}

static void compare_magnitudes(void) {
    static VocoderComplex freq[VOCODE_FFT];
    static float a[VOCODE_FFT/2+1], b[VOCODE_FFT/2+1], c[VOCODE_FFT/2+1];
    fill_complex(freq,VOCODE_FFT,2.0);
    freq[0].re=1e150; freq[VOCODE_FFT/2].im=-1e150;
    vocoder_magnitudes_c(a,freq);
#if VOCODE_TAILS_HAVE_AVX2
    vocoder_magnitudes_avx2(b,freq);
    assert(memcmp(a,b,sizeof(a))==0);
#endif
    vocoder_magnitudes(c,freq);
    assert(memcmp(a,c,sizeof(a))==0);
}

static void compare_mirror(void) {
    static VocoderComplex a[VOCODE_FFT], b[VOCODE_FFT], c[VOCODE_FFT];
    fill_complex(a,VOCODE_FFT,2.0);
    memcpy(b,a,sizeof(a));
    memcpy(c,a,sizeof(a));
    vocoder_mirror_c(a);
#if VOCODE_TAILS_HAVE_AVX2
    vocoder_mirror_avx2(b);
    assert(memcmp(a,b,sizeof(a))==0);
#endif
    vocoder_mirror(c);
    assert(memcmp(a,c,sizeof(a))==0);
}

static void compare_ola(void) {
    static VocoderComplex shifted[VOCODE_FFT];
    static float hann[VOCODE_FFT], base[VOCODE_FFT];
    static float a[VOCODE_FFT], b[VOCODE_FFT], c[VOCODE_FFT];
    fill_complex(shifted,VOCODE_FFT,2.0);
    fill_float(hann,VOCODE_FFT,2.0);
    fill_float(base,VOCODE_FFT,4.0);
    memcpy(a,base,sizeof(a));
    memcpy(b,base,sizeof(b));
    memcpy(c,base,sizeof(c));
    vocoder_ola_c(a,shifted,hann);
#if VOCODE_TAILS_HAVE_AVX2
    vocoder_ola_avx2(b,shifted,hann);
    assert(memcmp(a,b,sizeof(a))==0);
#endif
    vocoder_ola(c,shifted,hann);
    assert(memcmp(a,c,sizeof(a))==0);
}

static void benchmark(void) {
#if VOCODE_TAILS_HAVE_AVX2
    static float windowed[VOCODE_FFT], input[VOCODE_FFT], hann[VOCODE_FFT];
    static float magnitudes[VOCODE_FFT/2+1];
    static VocoderComplex freq[VOCODE_FFT], shifted[VOCODE_FFT], sum[VOCODE_FFT];
    static float out[VOCODE_FFT];
    enum { N=2000 };
    fill_float(input,VOCODE_FFT,2.0);
    fill_float(hann,VOCODE_FFT,2.0);
    fill_complex(freq,VOCODE_FFT,2.0);
    volatile double sink=0;
    struct timespec t0, t1, t2;
    clock_gettime(CLOCK_MONOTONIC,&t0);
    for (int i=0;i<N;i++) {
        vocoder_window_c(windowed,input,hann);
        vocoder_magnitudes_c(magnitudes,freq);
        sink+=windowed[0]+magnitudes[0];
    }
    clock_gettime(CLOCK_MONOTONIC,&t1);
    for (int i=0;i<N;i++) {
        vocoder_window_avx2(windowed,input,hann);
        vocoder_magnitudes_avx2(magnitudes,freq);
        sink+=windowed[0]+magnitudes[0];
    }
    clock_gettime(CLOCK_MONOTONIC,&t2);
    double c_ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)/1e6;
    double v_ms=(t2.tv_sec-t1.tv_sec)*1e3+(t2.tv_nsec-t1.tv_nsec)/1e6;
    (void)shifted; (void)sum; (void)out;
    printf("vocoder tails window+magnitudes: C %.3f ms, AVX2 %.3f ms (%.2fx)\n",
        c_ms,v_ms,v_ms>0?c_ms/v_ms:0);
    (void)sink;
#endif
}

int main(void) {
    for (int i=0;i<20;i++) {
        compare_window();
        compare_magnitudes();
        compare_mirror();
        compare_ola();
    }
    benchmark();
    puts("vocoder tail kernels match their C references bit-exact ok");
    return 0;
}
